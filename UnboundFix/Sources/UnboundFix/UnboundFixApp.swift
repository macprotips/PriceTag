import AppKit
import SwiftUI

@main
struct UnboundFixApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup("Unbound Fix") {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
