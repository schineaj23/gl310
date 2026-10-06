// gl310probe-cam - open the "GL310 HDMI" camera like any app would (AVFoundation)
// and report how stale frames are on delivery. The extension stamps each frame's
// presentation time with the host time at which it sent it, so
// now - PTS is the extension -> app delivery delay.
//
//   macos/build/gl310probe-cam [seconds]
import AVFoundation
import CoreMedia
import Foundation

func err(_ s: String) { FileHandle.standardError.write((s + "\n").data(using: .utf8)!) }

final class Probe: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var n = 0, sum = 0.0, mx = 0.0, total = 0
    var last = Date(), t0 = Date()

    func captureOutput(_ output: AVCaptureOutput, didOutput sbuf: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        let pts = CMSampleBufferGetPresentationTimeStamp(sbuf)
        let now = CMClockGetTime(CMClockGetHostTimeClock())
        let ms = (now.seconds - pts.seconds) * 1000
        n += 1; total += 1; sum += ms; mx = max(mx, ms)
        if Date().timeIntervalSince(last) >= 10 {
            print(String(format: "%5.0f s  %3d frames (%.1f fps)  delivery delay avg %6.1f ms  max %6.1f ms",
                         Date().timeIntervalSince(t0), n,
                         Double(n) / Date().timeIntervalSince(last), sum / Double(n), mx))
            fflush(stdout)
            n = 0; sum = 0; mx = 0; last = Date()
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didDrop sbuf: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        print("dropped a late frame"); fflush(stdout)
    }
}

let secs = Double(CommandLine.arguments.dropFirst().first ?? "60") ?? 60
let devices = AVCaptureDevice.DiscoverySession(deviceTypes: [.external], mediaType: .video,
                                               position: .unspecified).devices
guard let cam = devices.first(where: { $0.localizedName == GL310.deviceName }) else {
    err("no \"\(GL310.deviceName)\" camera; found: \(devices.map(\.localizedName))")
    exit(1)
}
let session = AVCaptureSession()
let input = try AVCaptureDeviceInput(device: cam)
session.addInput(input)
let out = AVCaptureVideoDataOutput()
let probe = Probe()
out.setSampleBufferDelegate(probe, queue: DispatchQueue(label: "probe"))
session.addOutput(out)
session.startRunning()
err("probing \"\(cam.localizedName)\" for \(Int(secs)) s")
DispatchQueue.main.asyncAfter(deadline: .now() + secs) {
    session.stopRunning()
    exit(0)
}
dispatchMain()
