// Identifiers shared by the camera extension and the feeder. The feeder finds the
// device by UID (the extension's deviceID, as a string) and picks the sink stream.
import Foundation

enum GL310 {
    static let deviceName  = "GL310 HDMI"
    static let deviceUID   = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6B"
    static let sourceUID   = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6C"
    static let sinkUID     = "7E1F3C2A-5B6D-4E8F-9A0B-1C2D3E4F5A6D"

    // Frame sizes the camera can present, all 30 fps NV12 (video range):
    //   1920x1080  full 16:9 frame from the card (1080p mode)
    //   1440x1080  4:3 crop of it - the picture area of a 4:3 source pillarboxed
    //              in 16:9 (e.g. a camera that outputs 1080i with side bars)
    //   1280x720   full frame in the card's 720p mode
    //    960x720   4:3 crop of that
    static let sizes: [(w: Int32, h: Int32)] = [(1920, 1080), (1440, 1080), (1280, 720), (960, 720)]
    static let width: Int32  = 1920   // default until frames say otherwise
    static let height: Int32 = 1080
    static let fps: Int32    = 30
}
