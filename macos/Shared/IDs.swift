// Identifiers shared by the camera extension and the feeder. The feeder finds the
// device by UID (the extension's deviceID, as a string) and picks the sink stream.
import Foundation

enum GL310 {
    static let deviceName  = "GL310 HDMI"
    static let deviceUID   = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6B"
    static let sourceUID   = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6C"
    static let sinkUID     = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6D"

    // The encoder's output: 1920x1080 at 30 fps, decoded to NV12 (video range).
    static let width: Int32  = 1920
    static let height: Int32 = 1080
    static let fps: Int32    = 30
}
