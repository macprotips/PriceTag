import AppKit
import SwiftUI
import UnboundFixKit

/// Renders every screen, in light and dark mode, to PNGs. Used by CI to check the UI:
/// `UNBOUNDFIX_SNAPSHOT_DIR=/some/folder "Unbound Fix.app/Contents/MacOS/UnboundFix"`.
@MainActor
enum Snapshots {
    static func render(to folder: URL) {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let game = URL(fileURLWithPath: NSHomeDirectory())
            .appendingPathComponent("Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Need for Speed Unbound/Engine.Render.Core2.PlatformPcDx12.dll")
        let backup = Patcher.defaultBackupDirectory
            .appendingPathComponent("2026-10-10 at 17.00.00/Engine.Render.Core2.PlatformPcDx12.dll")
        let hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

        let screens: [(String, FixModel.Phase, URL?)] = [
            ("1-waiting", .waiting, nil),
            ("2-waiting-recent", .waiting, game),
            ("3-searching", .searching, nil),
            ("4-working", .working("Backing up and applying the fix…"), nil),
            ("5-ready", .ready(file: game), nil),
            ("6-fixed", .fixed(file: game, report: ApplyReport(backup: backup, reusedBackup: false)), nil),
            ("7-already-fixed", .alreadyFixed(file: game, canUndo: true), nil),
            ("8-undone", .undone(file: game), nil),
            ("9-unsupported", .problem(title: PatchError.unsupportedVersion(sha256: hash).title!,
                                       message: PatchError.unsupportedVersion(sha256: hash).localizedDescription,
                                       detail: PatchError.unsupportedVersion(sha256: hash).detail), nil),
            ("10-running", .problem(title: "The fix wasn't applied",
                                    message: "Need for Speed Unbound is still running. Quit the game, then try again. Nothing was changed.",
                                    detail: nil), nil),
        ]

        for (name, phase, recent) in screens {
            let model = FixModel()
            model.phase = phase
            model.recentFile = recent
            for dark in [false, true] {
                write(ContentView(model: model), size: ContentView.size, dark: dark,
                      to: folder.appendingPathComponent("\(name)-\(dark ? "dark" : "light").png"))
            }
        }
        for dark in [false, true] {
            write(HelpView(), size: CGSize(width: 500, height: 480), dark: dark,
                  to: folder.appendingPathComponent("11-help-\(dark ? "dark" : "light").png"))
        }
    }

    private static func write<V: View>(_ view: V, size: CGSize, dark: Bool, to url: URL) {
        let hosting = NSHostingView(rootView: view)
        hosting.frame = CGRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: hosting.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        window.backgroundColor = dark ? NSColor(white: 0.16, alpha: 1) : .white
        window.contentView = hosting
        window.setFrameOrigin(CGPoint(x: -10_000, y: -10_000))
        window.orderFront(nil)
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        hosting.layoutSubtreeIfNeeded()

        guard let rep = hosting.bitmapImageRepForCachingDisplay(in: hosting.bounds) else { return }
        hosting.cacheDisplay(in: hosting.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: url)
        window.orderOut(nil)
    }
}
