import AppKit
import SwiftUI
import SM2FixKit

@main
enum Entry {
    static func main() {
        let args = CommandLine.arguments
        if args.contains("--selftest") {
            exit(SelfTest.run())
        }
        if let i = args.firstIndex(of: "--snapshots"), i + 1 < args.count {
            exit(Snapshots.run(into: URL(fileURLWithPath: args[i + 1])))
        }
        SM2FixApp.main()
    }
}

struct SM2FixApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel.shared

    var body: some Scene {
        Window("Spider-Man 2 T-Pose Fix", id: "main") {
            ContentView(model: model)
        }
        .windowResizability(.contentSize)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(replacing: .help) {
                Link("Spider-Man 2 T-Pose Fix Help",
                     destination: URL(string: "https://github.com/macprotips/PriceTag/tree/claude/elegant-dijkstra-9yhhvx/spiderman2-tpose-fix")!)
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    /// A folder dropped on the app's icon (Dock or Finder).
    func application(_ application: NSApplication, open urls: [URL]) {
        if let url = urls.first {
            AppModel.shared.open(url)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

/// `--selftest`: checks the bundled fix files and does an install/remove round trip in a
/// temporary folder. Used by CI on the built app.
enum SelfTest {
    static func run() -> Int32 {
        do {
            let payload = try AppModel.loadPayload()
            print("payload: \(payload.dll.path)")
            print("shaders: \(payload.shaders.map(\.lastPathComponent).joined(separator: ", "))")
            let fm = FileManager.default
            let game = fm.temporaryDirectory.appendingPathComponent("sm2fix-selftest-\(UUID().uuidString)")
            try fm.createDirectory(at: game, withIntermediateDirectories: true)
            defer { try? fm.removeItem(at: game) }
            try Data("MZ".utf8).write(to: game.appendingPathComponent(GameFinder.exeName))
            let installer = Installer(payload: payload, env: SystemEnvironment())
            let install = GameInstall(folder: game, bottle: nil, candidates: [])
            _ = try installer.install(install)
            guard installer.status(of: game) == .installed else {
                print("FAIL: status after install is \(installer.status(of: game))")
                return 1
            }
            _ = try installer.uninstall(install)
            guard installer.status(of: game) == .notInstalled else {
                print("FAIL: status after removal is \(installer.status(of: game))")
                return 1
            }
            print("selftest ok")
            return 0
        } catch {
            print("FAIL: \(error.localizedDescription)")
            return 1
        }
    }
}
