// gl310feed - push decoded frames into the GL310 camera extension's sink stream.
//
// Reads raw NV12 1920x1080 frames on stdin (ffmpeg -f rawvideo -pix_fmt nv12) and
// enqueues each one, as an IOSurface-backed CVPixelBuffer, on the sink stream's
// CMSimpleQueue - the same mechanism OBS uses to drive its virtual camera.
//
//   tools/gl310cam                      # the whole chain
//   ... | gl310feed --dry-run           # read and time frames, no camera needed
import CoreMedia
import CoreMediaIO
import CoreVideo
import Foundation

let W = Int(GL310.width), H = Int(GL310.height)
let frameBytes = W * H * 3 / 2
let dryRun = CommandLine.arguments.contains("--dry-run")

func err(_ s: String) { FileHandle.standardError.write((s + "\n").data(using: .utf8)!) }

var stopRequested = false
signal(SIGINT) { _ in stopRequested = true }
signal(SIGTERM) { _ in stopRequested = true }
signal(SIGPIPE, SIG_IGN)

// MARK: CMIO lookup

func propAddr(_ sel: Int) -> CMIOObjectPropertyAddress {
    CMIOObjectPropertyAddress(mSelector: CMIOObjectPropertySelector(sel),
                              mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
                              mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
}

func objectList(_ obj: CMIOObjectID, _ sel: Int) -> [CMIOObjectID] {
    var addr = propAddr(sel)
    var size: UInt32 = 0
    guard CMIOObjectGetPropertyDataSize(obj, &addr, 0, nil, &size) == noErr, size > 0 else {
        return []
    }
    var ids = [CMIOObjectID](repeating: 0, count: Int(size) / MemoryLayout<CMIOObjectID>.size)
    var used: UInt32 = 0
    guard CMIOObjectGetPropertyData(obj, &addr, 0, nil, size, &used, &ids) == noErr else {
        return []
    }
    return ids
}

func stringProp(_ obj: CMIOObjectID, _ sel: Int) -> String? {
    var addr = propAddr(sel)
    var value: Unmanaged<CFString>?
    var used: UInt32 = 0
    let size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard CMIOObjectGetPropertyData(obj, &addr, 0, nil, size, &used, &value) == noErr,
          let value else { return nil }
    return value.takeRetainedValue() as String
}

func uint32Prop(_ obj: CMIOObjectID, _ sel: Int) -> UInt32? {
    var addr = propAddr(sel)
    var value: UInt32 = 0
    var used: UInt32 = 0
    guard CMIOObjectGetPropertyData(obj, &addr, 0, nil, 4, &used, &value) == noErr else {
        return nil
    }
    return value
}

// Extension devices only show up once a client opts in to screen-capture-style
// "DAL" devices; without this the device list can be empty.
func allowExtensionDevices() {
    var addr = propAddr(Int(kCMIOHardwarePropertyAllowScreenCaptureDevices))
    var allow: UInt32 = 1
    CMIOObjectSetPropertyData(CMIOObjectID(kCMIOObjectSystemObject), &addr, 0, nil, 4, &allow)
}

struct Sink {
    let device: CMIOObjectID
    let stream: CMIOStreamID
    let queue: CMSimpleQueue
}

func openSink() -> Sink? {
    allowExtensionDevices()
    var device: CMIOObjectID = 0
    for _ in 0..<50 {   // the device list fills in asynchronously at startup
        for d in objectList(CMIOObjectID(kCMIOObjectSystemObject),
                            Int(kCMIOHardwarePropertyDevices))
        where stringProp(d, Int(kCMIODevicePropertyDeviceUID)) == GL310.deviceUID {
            device = d
        }
        if device != 0 { break }
        usleep(100_000)
    }
    guard device != 0 else {
        err("gl310feed: no \"\(GL310.deviceName)\" camera - is the extension activated?")
        return nil
    }
    // Direction 0 is output (host -> device): that is the sink.
    let streams = objectList(device, Int(kCMIODevicePropertyStreams))
    guard let stream = streams.first(where: {
        uint32Prop($0, Int(kCMIOStreamPropertyDirection)) == 0
    }) ?? (streams.count > 1 ? streams[1] : nil) else {
        err("gl310feed: the camera has no sink stream")
        return nil
    }
    var q: Unmanaged<CMSimpleQueue>?
    let r = CMIOStreamCopyBufferQueue(stream, { _, _, _ in }, nil, &q)
    guard r == noErr, let q else {
        err("gl310feed: CMIOStreamCopyBufferQueue failed (\(r))")
        return nil
    }
    let s = CMIODeviceStartStream(device, stream)
    guard s == noErr else {
        err("gl310feed: CMIODeviceStartStream failed (\(s))")
        return nil
    }
    return Sink(device: device, stream: stream, queue: q.takeRetainedValue())
}

// MARK: frames

func makePool() -> CVPixelBufferPool? {
    let attrs: [CFString: Any] = [
        kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        kCVPixelBufferWidthKey: W,
        kCVPixelBufferHeightKey: H,
        kCVPixelBufferIOSurfacePropertiesKey: [:],   // required to cross processes
    ]
    var pool: CVPixelBufferPool?
    CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attrs as CFDictionary, &pool)
    return pool
}

func fill(_ pb: CVPixelBuffer, from frame: UnsafeRawPointer) {
    CVPixelBufferLockBaseAddress(pb, [])
    for plane in 0..<2 {
        let rows = plane == 0 ? H : H / 2
        let dst = CVPixelBufferGetBaseAddressOfPlane(pb, plane)!
        let stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane)
        let src = frame + (plane == 0 ? 0 : W * H)
        if stride == W {
            memcpy(dst, src, W * rows)
        } else {
            for r in 0..<rows { memcpy(dst + r * stride, src + r * W, W) }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, [])
}

func sampleBuffer(_ pb: CVPixelBuffer, pts: CMTime) -> CMSampleBuffer? {
    var desc: CMFormatDescription?
    CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault,
                                                 imageBuffer: pb, formatDescriptionOut: &desc)
    guard let desc else { return nil }
    var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: GL310.fps),
                                    presentationTimeStamp: pts, decodeTimeStamp: .invalid)
    var sbuf: CMSampleBuffer?
    CMSampleBufferCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pb,
                                       dataReady: true, makeDataReadyCallback: nil,
                                       refcon: nil, formatDescription: desc,
                                       sampleTiming: &timing, sampleBufferOut: &sbuf)
    return sbuf
}

func readFrame(_ buf: UnsafeMutableRawPointer) -> Bool {
    var got = 0
    while got < frameBytes {
        let n = read(0, buf + got, frameBytes - got)
        if n <= 0 { return false }
        got += n
    }
    return true
}

// MARK: main loop

guard let pool = makePool() else { err("gl310feed: cannot create pixel buffer pool"); exit(1) }
let sink = dryRun ? nil : openSink()
if !dryRun && sink == nil { exit(1) }
err(dryRun ? "gl310feed: dry run, reading frames only"
           : "gl310feed: feeding \"\(GL310.deviceName)\"")

let frame = UnsafeMutableRawPointer.allocate(byteCount: frameBytes, alignment: 64)
var frames = 0, dropped = 0
let t0 = Date()
while !stopRequested && readFrame(frame) {
    frames += 1
    guard let sink else { continue }
    // The sink queue is small; if the extension is not draining it (no client is
    // watching the camera), drop rather than block the decoder upstream.
    if CMSimpleQueueGetCount(sink.queue) >= CMSimpleQueueGetCapacity(sink.queue) {
        dropped += 1
        continue
    }
    var pb: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb)
    guard let pb else { dropped += 1; continue }
    fill(pb, from: frame)
    guard let sbuf = sampleBuffer(pb, pts: CMClockGetTime(CMClockGetHostTimeClock())) else {
        dropped += 1
        continue
    }
    if CMSimpleQueueEnqueue(sink.queue, element: Unmanaged.passRetained(sbuf).toOpaque())
        != noErr {
        Unmanaged.passUnretained(sbuf).release()
        dropped += 1
    }
}
let secs = Date().timeIntervalSince(t0)
err(String(format: "gl310feed: %d frames in %.1f s (%.2f fps), %d dropped",
           frames, secs, Double(frames) / max(secs, 0.001), dropped))
if let sink { CMIODeviceStopStream(sink.device, sink.stream) }
