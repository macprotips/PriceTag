import AppKit
import SwiftUI

@main
struct UnboundFixApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = FixModel()

    var body: some Scene {
        Window("Unbound Fix", id: "main") {
            ContentView(model: model)
        }
        .windowResizability(.contentSize)
        .windowStyle(.hiddenTitleBar)
        .defaultPosition(.center)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Choose Folder…") { model.chooseFolder() }
                    .keyboardShortcut("o")
                    .disabled(model.isBusy)
                Divider()
                Button("Show Backups Folder") { model.showBackupsFolder() }
            }
            CommandGroup(replacing: .help) {
                Button("Unbound Fix Help") { model.showingHelp = true }
                    .keyboardShortcut("?")
            }
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        if let folder = ProcessInfo.processInfo.environment["UNBOUNDFIX_SNAPSHOT_DIR"] {
            Snapshots.render(to: URL(fileURLWithPath: folder))
            exit(0)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
