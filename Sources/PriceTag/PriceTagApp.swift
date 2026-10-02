import AppKit
import SwiftUI

@main
struct PriceTagApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        Window("PriceTag", id: "main") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 860, minHeight: 560)
        }
        .defaultSize(width: 1040, height: 660)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(after: .saveItem) {
                Button("Save to Export Folder") { model.saveToExportFolder() }
                    .keyboardShortcut(.return, modifiers: .command)
                    .disabled(model.price == nil)
                Button("Save As…") { model.saveAs() }
                    .keyboardShortcut("s", modifiers: .command)
                    .disabled(model.price == nil)
                Button("Batch Export…") { model.showingBatch = true }
                    .keyboardShortcut("b", modifiers: .command)
                Divider()
                Button("Choose Export Folder…") { model.chooseExportFolder() }
                Button("Show Export Folder in Finder") { model.revealExportFolder() }
            }
            CommandGroup(after: .pasteboard) {
                Divider()
                Button("Copy Price Image") { model.copyToClipboard() }
                    .keyboardShortcut("c", modifiers: [.command, .shift])
                    .disabled(model.price == nil)
            }
            CommandMenu("Color") {
                ForEach(Array(TagColor.allCases.enumerated()), id: \.element) { index, color in
                    Button(color.name) { model.style.color = color }
                        .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .command)
                }
                Divider()
                Button("Switch Green / Red") { model.toggleGreenRed() }
                    .keyboardShortcut("r", modifiers: .command)
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Needed when launched with `swift run`, harmless inside the .app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
