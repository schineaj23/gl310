// GL310Camera.app - container for the camera extension. It has no UI; run it from
// a terminal to install or remove the extension:
//
//   /Applications/GL310Camera.app/Contents/MacOS/GL310Camera activate
//   /Applications/GL310Camera.app/Contents/MacOS/GL310Camera deactivate
//
// macOS only activates system extensions from apps in /Applications.
import Foundation
import SystemExtensions

final class Delegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        print("replacing \(existing.bundleShortVersion) with \(ext.bundleShortVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("Waiting for approval: System Settings > General > Login Items & Extensions"
              + " > Camera Extensions")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        print(result == .completed ? "done" : "done; takes effect after a reboot")
        exit(0)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let e = error as NSError
        print("failed: \(e.localizedDescription) (\(e.domain) \(e.code))")
        exit(1)
    }
}

let cmd = CommandLine.arguments.dropFirst().first ?? "activate"
let request: OSSystemExtensionRequest
switch cmd {
case "activate":
    request = .activationRequest(forExtensionWithIdentifier: GL310.extensionID, queue: .main)
case "deactivate":
    request = .deactivationRequest(forExtensionWithIdentifier: GL310.extensionID, queue: .main)
default:
    print("usage: GL310Camera activate|deactivate")
    exit(2)
}
let delegate = Delegate()
request.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(request)
dispatchMain()
