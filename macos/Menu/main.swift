// GL310Menu.app - menu-bar control for the GL310 HDMI camera.
//
// Starts and stops tools/gl310cam and picks its settings. Every choice offered here
// is one that has been verified on the card (see RE.md); the card is 30 fps only.
// Changing a setting while the camera is running restarts the stream (a gap of a
// few seconds), and apps showing the camera need to reopen it if the size changed.
import AppKit
import Darwin
import SwiftUI

enum Resolution: String, CaseIterable, Identifiable {
    case p1080 = "1080", p720 = "720"
    var id: String { rawValue }
    var label: String { self == .p1080 ? "1080p" : "720p" }
}

enum Aspect: String, CaseIterable, Identifiable {
    case wide = "16:9", standard = "4:3"
    var id: String { rawValue }
    var label: String { self == .wide ? "16:9 (full frame)" : "4:3 (crop side bars)" }
}

func outputSize(_ r: Resolution, _ a: Aspect) -> String {
    let h = r == .p1080 ? 1080 : 720
    let w = a == .wide ? h * 16 / 9 : h * 4 / 3
    return "\(w)x\(h)"
}

@MainActor
final class Pipeline: ObservableObject {
    enum State: Equatable { case stopped, starting, running, stopping, failed(String) }

    @Published var state: State = .stopped
    @Published var resolution: Resolution { didSet { settingChanged("res", resolution.rawValue) } }
    @Published var aspect: Aspect { didSet { settingChanged("aspect", aspect.rawValue) } }
    @Published var rate: Int { didSet { settingChanged("rate", rate) } }

    private var pid: pid_t = 0
    private var restartAfterStop = false
    private var loadingDefaults = true
    let logURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/gl310cam.log")

    init() {
        let d = UserDefaults.standard
        resolution = Resolution(rawValue: d.string(forKey: "res") ?? "") ?? .p1080
        aspect = Aspect(rawValue: d.string(forKey: "aspect") ?? "") ?? .wide
        let r = d.integer(forKey: "rate")
        rate = GL310Menu.rates.contains(r) ? r : 8000
        loadingDefaults = false
    }

    var running: Bool { state == .running || state == .starting }

    var status: String {
        switch state {
        case .stopped: return "Stopped"
        case .starting: return "Starting…"
        case .running: return "Live: \(outputSize(resolution, aspect)) @ 30 fps, \(rate / 1000) Mbps"
        case .stopping: return "Stopping…"
        case .failed(let why): return "Stopped: \(why)"
        }
    }

    private func settingChanged(_ key: String, _ value: Any) {
        guard !loadingDefaults else { return }
        UserDefaults.standard.set(value, forKey: key)
        if running { restartAfterStop = true; stop() }
    }

    func start() {
        guard pid == 0 else { return }
        let cam = GL310Menu.toolsDir + "/gl310cam"
        let cmd = "exec '\(cam)' --res \(resolution.rawValue) --aspect \(aspect.rawValue)"
            + " --rate \(rate) >/dev/null 2>>'\(logURL.path)'"
        try? "\n=== \(Date()) \(cmd)\n".appendLine(to: logURL)

        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP))
        posix_spawnattr_setpgroup(&attr, 0)   // own process group: signal it as one
        var child: pid_t = 0
        let argv: [UnsafeMutablePointer<CChar>?] = ["/bin/sh", "-c", cmd].map { strdup($0) } + [nil]
        let rc = posix_spawn(&child, "/bin/sh", nil, &attr, argv, environ)
        argv.forEach { free($0) }
        posix_spawnattr_destroy(&attr)
        guard rc == 0 else { state = .failed("could not launch gl310cam (\(rc))"); return }

        pid = child
        state = .starting
        let started = Date()
        // gl310live takes a few seconds to bring the card up before frames flow.
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
            if self?.state == .starting { self?.state = .running }
        }
        DispatchQueue.global().async { [weak self] in
            var status: Int32 = 0
            waitpid(child, &status, 0)
            DispatchQueue.main.async { self?.exited(after: Date().timeIntervalSince(started)) }
        }
    }

    func stop() {
        guard pid != 0 else { return }
        state = .stopping
        // SIGINT to the whole group: gl310start quiesces, stops the encoder and
        // reboots the card's firmware; ffmpeg and gl310feed exit on their own.
        kill(-pid, SIGINT)
    }

    private func exited(after seconds: TimeInterval) {
        let wasStopping = state == .stopping
        pid = 0
        if restartAfterStop {
            restartAfterStop = false
            state = .stopped
            start()
        } else if wasStopping {
            state = .stopped
        } else {
            state = .failed(lastLogError() ?? "exited after \(Int(seconds)) s - see log")
        }
    }

    private func lastLogError() -> String? {
        guard let text = try? String(contentsOf: logURL, encoding: .utf8) else { return nil }
        let lines = text.split(separator: "\n").suffix(40)
        let hit = lines.last { l in
            ["claim failed", "cannot open", "no \"", "must be", "failed"].contains { l.contains($0) }
        }
        return hit.map { $0.trimmingCharacters(in: .whitespaces) }
    }

    func quit() {
        let child = pid
        guard child != 0 else { NSApp.terminate(nil); return }
        restartAfterStop = false
        stop()
        // Give gl310start time to shut the card down cleanly before we exit.
        DispatchQueue.global().async {
            for _ in 0..<100 where kill(child, 0) == 0 { usleep(100_000) }
            DispatchQueue.main.async { NSApp.terminate(nil) }
        }
    }
}

extension String {
    func appendLine(to url: URL) throws {
        if let h = try? FileHandle(forWritingTo: url) {
            h.seekToEndOfFile(); h.write(data(using: .utf8)!); try h.close()
        } else {
            try write(to: url, atomically: true, encoding: .utf8)
        }
    }
}

@main
struct GL310MenuApp: App {
    @StateObject private var p = Pipeline()

    var body: some Scene {
        MenuBarExtra {
            Text(p.status)
            Divider()
            if p.running {
                Button("Stop Camera") { p.stop() }
            } else {
                Button("Start Camera") { p.start() }.disabled(p.state == .stopping)
            }
            Divider()
            Picker("Resolution", selection: $p.resolution) {
                ForEach(Resolution.allCases) { Text($0.label).tag($0) }
            }
            Picker("Aspect", selection: $p.aspect) {
                ForEach(Aspect.allCases) { Text($0.label).tag($0) }
            }
            Picker("Bitrate", selection: $p.rate) {
                ForEach(GL310Menu.rates, id: \.self) { Text("\($0 / 1000) Mbps").tag($0) }
            }
            Text("Frame rate: 30 fps (fixed by the card)")
            Divider()
            Button("Show Log") { NSWorkspace.shared.open(p.logURL) }
            Button("Quit") { p.quit() }.keyboardShortcut("q")
        } label: {
            Image(systemName: p.running ? "video.fill" : "video.slash")
        }
    }
}
