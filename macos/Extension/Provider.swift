// The camera extension: one device, "GL310 HDMI", with two streams.
//
//   source  what every camera client (FaceTime, Zoom, browsers, OBS) reads
//   sink    where gl310feed pushes decoded frames
//
// This is the same split OBS's virtual camera uses. The extension runs sandboxed as
// a system user, so it does not touch USB itself: the card is driven by the
// user-space tools that already work (tools/gl310live), and frames arrive here
// through the sink. When nothing has arrived for half a second the source shows
// a flat placeholder, so clients see "no signal" rather than a frozen picture.
//
// Frame size follows the frames: the sink accepts any size in GL310.sizes, and when
// the size arriving changes (gl310cam --res / --aspect, or the menu-bar app), the
// source stream is rebuilt with that size as its only format, so apps see the
// picture natively - e.g. 1440x1080 4:3 with no bars. Apps already showing the
// camera need to reopen it. The last size is remembered across restarts.
import CoreMedia
import CoreMediaIO
import CoreVideo
import Foundation
import IOKit.audio
import os.log

let log = Logger(subsystem: GL310.extensionID, category: "camera")

final class DeviceSource: NSObject, CMIOExtensionDeviceSource {
    private(set) var device: CMIOExtensionDevice!
    private var sourceStream: CMIOExtensionStream!
    private var sinkStream: CMIOExtensionStream!
    private var sourceSrc: SourceStreamSource!
    private var sinkSrc: SinkStreamSource!

    private var curW: Int32
    private var curH: Int32
    private let frameDuration = CMTime(value: 1, timescale: GL310.fps)
    private let queue = DispatchQueue(label: "gl310.camera", qos: .userInteractive)

    private var sourceClients = 0
    private var placeholderTimer: DispatchSourceTimer?
    private var placeholder: CVPixelBuffer?
    private var lastSinkFrameNs: UInt64 = 0

    private var sinkClient: CMIOExtensionClient?
    private var sinkRunning = false
    private var consumeInFlight = false
    private var sinkTimer: DispatchSourceTimer?

    private func format(_ w: Int32, _ h: Int32) -> CMIOExtensionStreamFormat {
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault,
                                       codecType: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                       width: w, height: h, extensions: nil,
                                       formatDescriptionOut: &desc)
        return CMIOExtensionStreamFormat(formatDescription: desc!,
                                         maxFrameDuration: frameDuration,
                                         minFrameDuration: frameDuration,
                                         validFrameDurations: nil)
    }

    init(localizedName: String) {
        let saved = UserDefaults.standard.string(forKey: "size") ?? ""
        let parts = saved.split(separator: "x").compactMap { Int32($0) }
        if parts.count == 2, GL310.sizes.contains(where: { $0.w == parts[0] && $0.h == parts[1] }) {
            curW = parts[0]; curH = parts[1]
        } else {
            curW = GL310.width; curH = GL310.height
        }
        super.init()

        device = CMIOExtensionDevice(localizedName: localizedName,
                                     deviceID: UUID(uuidString: GL310.deviceUID)!,
                                     legacyDeviceID: GL310.deviceUID, source: self)
        sourceSrc = SourceStreamSource(device: self, formats: [format(curW, curH)])
        sinkSrc = SinkStreamSource(device: self, formats: GL310.sizes.map { format($0.w, $0.h) })
        sourceStream = CMIOExtensionStream(localizedName: "GL310 Video",
                                           streamID: UUID(uuidString: GL310.sourceUID)!,
                                           direction: .source, clockType: .hostTime,
                                           source: sourceSrc)
        sinkStream = CMIOExtensionStream(localizedName: "GL310 Sink",
                                         streamID: UUID(uuidString: GL310.sinkUID)!,
                                         direction: .sink, clockType: .hostTime,
                                         source: sinkSrc)
        do {
            try device.addStream(sourceStream)
            try device.addStream(sinkStream)
        } catch {
            fatalError("addStream: \(error)")
        }
        placeholder = makePlaceholder()
    }

    // Replace the source stream with one whose only format is w x h. Runs on queue.
    private func rebuildSource(_ w: Int32, _ h: Int32) {
        log.info("frame size \(w)x\(h): rebuilding source stream")
        placeholderTimer?.cancel()
        placeholderTimer = nil
        sourceClients = 0
        do { try device.removeStream(sourceStream) } catch {
            log.error("removeStream: \(error.localizedDescription)")
        }
        curW = w; curH = h
        UserDefaults.standard.set("\(w)x\(h)", forKey: "size")
        sourceSrc = SourceStreamSource(device: self, formats: [format(w, h)])
        sourceStream = CMIOExtensionStream(localizedName: "GL310 Video",
                                           streamID: UUID(uuidString: GL310.sourceUID)!,
                                           direction: .source, clockType: .hostTime,
                                           source: sourceSrc)
        do { try device.addStream(sourceStream) } catch {
            log.error("addStream: \(error.localizedDescription)")
        }
        placeholder = makePlaceholder()
    }

    var availableProperties: Set<CMIOExtensionProperty> { [.deviceTransportType, .deviceModel] }

    func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionDeviceProperties {
        let p = CMIOExtensionDeviceProperties(dictionary: [:])
        if properties.contains(.deviceTransportType) {
            p.transportType = kIOAudioDeviceTransportTypeVirtual
        }
        if properties.contains(.deviceModel) { p.model = "GL310 / C835 HDMI capture" }
        return p
    }

    func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}

    // MARK: source side

    func startSource() {
        queue.async { [self] in
            self.sourceClients += 1
            guard self.placeholderTimer == nil else { return }
            let t = DispatchSource.makeTimerSource(queue: self.queue)
            t.schedule(deadline: .now(), repeating: 1.0 / Double(GL310.fps))
            t.setEventHandler { [weak self = self] in self?.placeholderTick() }
            t.resume()
            self.placeholderTimer = t
        }
    }

    func stopSource() {
        queue.async {
            self.sourceClients = max(0, self.sourceClients - 1)
            if self.sourceClients == 0 {
                self.placeholderTimer?.cancel()
                self.placeholderTimer = nil
            }
        }
    }

    private func placeholderTick() {
        let now = nowNs()
        if now &- lastSinkFrameNs < 500_000_000 { return }   // live frames are flowing
        if let pb = placeholder { send(pb, hostNs: now) }
    }

    private func send(_ pb: CVPixelBuffer, hostNs: UInt64) {
        guard sourceClients > 0 else { return }
        var desc: CMFormatDescription?
        CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault,
                                                     imageBuffer: pb, formatDescriptionOut: &desc)
        guard let desc else { return }
        var timing = CMSampleTimingInfo(duration: frameDuration,
                                        presentationTimeStamp: CMTime(value: CMTimeValue(hostNs),
                                                                      timescale: 1_000_000_000),
                                        decodeTimeStamp: .invalid)
        var sbuf: CMSampleBuffer?
        let err = CMSampleBufferCreateForImageBuffer(allocator: kCFAllocatorDefault,
                                                     imageBuffer: pb, dataReady: true,
                                                     makeDataReadyCallback: nil, refcon: nil,
                                                     formatDescription: desc,
                                                     sampleTiming: &timing,
                                                     sampleBufferOut: &sbuf)
        guard err == noErr, let sbuf else { return }
        sourceStream.send(sbuf, discontinuity: [], hostTimeInNanoseconds: hostNs)
    }

    // MARK: sink side

    func startSink(client: CMIOExtensionClient?) {
        queue.async { [self] in
            self.sinkClient = client
            self.sinkRunning = true
            guard self.sinkTimer == nil else { return }
            // Poll at twice the frame rate; consumeSampleBuffer completes immediately
            // with an error when the feeder has nothing queued.
            let t = DispatchSource.makeTimerSource(queue: self.queue)
            t.schedule(deadline: .now(), repeating: 0.5 / Double(GL310.fps))
            t.setEventHandler { [weak self = self] in self?.consumeOne() }
            t.resume()
            self.sinkTimer = t
        }
    }

    func stopSink() {
        queue.async {
            self.sinkRunning = false
            self.sinkTimer?.cancel()
            self.sinkTimer = nil
            self.sinkClient = nil
        }
    }

    private func consumeOne() {
        guard sinkRunning, !consumeInFlight, let client = sinkClient else { return }
        consumeInFlight = true
        sinkStream.consumeSampleBuffer(from: client) { [weak self] sbuf, seq, _, _, _ in
            guard let self else { return }
            self.queue.async {
                self.consumeInFlight = false
                guard let sbuf, let pb = CMSampleBufferGetImageBuffer(sbuf) else { return }
                let w = Int32(CVPixelBufferGetWidth(pb)), h = Int32(CVPixelBufferGetHeight(pb))
                if w != self.curW || h != self.curH {
                    guard GL310.sizes.contains(where: { $0.w == w && $0.h == h }) else { return }
                    self.rebuildSource(w, h)
                }
                let now = nowNs()
                self.lastSinkFrameNs = now
                self.send(pb, hostNs: now)
                self.sinkStream.notifyScheduledOutputChanged(
                    CMIOExtensionScheduledOutput(sequenceNumber: seq, hostTimeInNanoseconds: now))
            }
        }
    }

    // A dark gray frame with a lighter band across the middle: unmistakably "no
    // input" without needing text rendering in NV12.
    private func makePlaceholder() -> CVPixelBuffer? {
        var pb: CVPixelBuffer?
        let attrs: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:]]
        CVPixelBufferCreate(kCFAllocatorDefault, Int(curW), Int(curH),
                            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            attrs as CFDictionary, &pb)
        guard let pb else { return nil }
        CVPixelBufferLockBaseAddress(pb, [])
        let h = Int(curH)
        let y = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!.assumingMemoryBound(to: UInt8.self)
        let yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
        for row in 0..<h {
            let band = row > h * 9 / 20 && row < h * 11 / 20
            memset(y + row * yStride, band ? 60 : 32, yStride)
        }
        let uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1)!
        memset(uv, 128, CVPixelBufferGetBytesPerRowOfPlane(pb, 1) * h / 2)
        CVPixelBufferUnlockBaseAddress(pb, [])
        return pb
    }
}

func nowNs() -> UInt64 {
    UInt64(CMClockGetTime(CMClockGetHostTimeClock()).seconds * 1_000_000_000)
}

final class SourceStreamSource: NSObject, CMIOExtensionStreamSource {
    private weak var device: DeviceSource?
    let formats: [CMIOExtensionStreamFormat]

    init(device: DeviceSource, formats: [CMIOExtensionStreamFormat]) {
        self.device = device
        self.formats = formats
    }
    var availableProperties: Set<CMIOExtensionProperty> {
        [.streamActiveFormatIndex, .streamFrameDuration]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties {
        let p = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { p.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration) {
            p.frameDuration = CMTime(value: 1, timescale: GL310.fps)
        }
        return p
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {}
    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }
    func startStream() throws { device?.startSource() }
    func stopStream() throws { device?.stopSource() }
}

final class SinkStreamSource: NSObject, CMIOExtensionStreamSource {
    private weak var device: DeviceSource?
    let formats: [CMIOExtensionStreamFormat]
    private var client: CMIOExtensionClient?

    init(device: DeviceSource, formats: [CMIOExtensionStreamFormat]) {
        self.device = device
        self.formats = formats
    }
    var availableProperties: Set<CMIOExtensionProperty> {
        [.streamActiveFormatIndex, .streamFrameDuration, .streamSinkBufferQueueSize,
         .streamSinkBuffersRequiredForStartup, .streamSinkBufferUnderrunCount,
         .streamSinkEndOfData]
    }

    func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionStreamProperties {
        let p = CMIOExtensionStreamProperties(dictionary: [:])
        if properties.contains(.streamActiveFormatIndex) { p.activeFormatIndex = 0 }
        if properties.contains(.streamFrameDuration) {
            p.frameDuration = CMTime(value: 1, timescale: GL310.fps)
        }
        if properties.contains(.streamSinkBufferQueueSize) { p.sinkBufferQueueSize = 4 }
        if properties.contains(.streamSinkBuffersRequiredForStartup) {
            p.sinkBuffersRequiredForStartup = 1
        }
        return p
    }

    func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {}

    func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool {
        self.client = client
        return true
    }

    func startStream() throws { device?.startSink(client: client) }
    func stopStream() throws { device?.stopSink() }
}

final class ProviderSource: NSObject, CMIOExtensionProviderSource {
    private(set) var provider: CMIOExtensionProvider!
    private var deviceSource: DeviceSource!

    init(clientQueue: DispatchQueue?) {
        super.init()
        provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
        deviceSource = DeviceSource(localizedName: GL310.deviceName)
        do {
            try provider.addDevice(deviceSource.device)
        } catch {
            fatalError("addDevice: \(error)")
        }
    }

    func connect(to client: CMIOExtensionClient) throws {}
    func disconnect(from client: CMIOExtensionClient) {}

    var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }

    func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws
        -> CMIOExtensionProviderProperties {
        let p = CMIOExtensionProviderProperties(dictionary: [:])
        if properties.contains(.providerManufacturer) { p.manufacturer = "gl310 (unofficial)" }
        return p
    }

    func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}
