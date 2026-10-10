import AppKit
import Foundation
import UnboundFixKit

@MainActor
final class FixModel: ObservableObject {
    enum Phase {
        case waiting
        case working(String)
        case ready(file: URL)
        case alreadyFixed(file: URL, canUndo: Bool)
        case fixed(file: URL, backup: URL)
        case undone(file: URL)
        case problem(String, detail: String?)
    }

    @Published var phase: Phase = .waiting

    let patcher = Patcher(spec: .unboundBlackScreen, backupDirectory: Patcher.defaultBackupDirectory)

    func load(_ url: URL) {
        phase = .working("Looking for the game file…")
        let patcher = patcher
        run {
            let file = try patcher.findTarget(in: url)
            switch try patcher.state(of: file) {
            case .original:
                return .ready(file: file)
            case .patched:
                return .alreadyFixed(file: file, canUndo: patcher.existingBackup() != nil)
            case .unsupported(let hash):
                return .problem(PatchError.unsupportedVersion(sha256: hash).localizedDescription,
                                detail: "Found: \(file.path)\nSHA-256: \(hash)")
            }
        }
    }

    func applyFix(to file: URL) {
        guard !Self.isGameRunning() else {
            phase = .problem("Quit Need for Speed Unbound first, then try again. Nothing was changed.", detail: nil)
            return
        }
        phase = .working("Backing up and applying the fix…")
        let patcher = patcher
        run {
            let backup = try patcher.apply(to: file)
            return .fixed(file: file, backup: backup)
        }
    }

    func undoFix(on file: URL) {
        guard !Self.isGameRunning() else {
            phase = .problem("Quit Need for Speed Unbound first, then try again. Nothing was changed.", detail: nil)
            return
        }
        phase = .working("Putting the original file back…")
        let patcher = patcher
        run {
            try patcher.restore(file)
            return .undone(file: file)
        }
    }

    func startOver() {
        phase = .waiting
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.message = "Choose your Need for Speed Unbound game folder (or your CrossOver bottle)"
        panel.prompt = "Choose"
        let bottles = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/CrossOver/Bottles")
        if FileManager.default.fileExists(atPath: bottles.path) {
            panel.directoryURL = bottles
        }
        if panel.runModal() == .OK, let url = panel.url {
            load(url)
        }
    }

    func showInFinder(_ url: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    /// Runs file work off the main thread and shows the outcome.
    private func run(_ work: @escaping @Sendable () throws -> Phase) {
        Task.detached(priority: .userInitiated) {
            let result: Phase
            do {
                result = try work()
            } catch {
                result = .problem(error.localizedDescription, detail: nil)
            }
            await MainActor.run { self.phase = result }
        }
    }

    /// Best-effort check for the game's process, which runs under CrossOver/Wine.
    private static func isGameRunning() -> Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/ps")
        process.arguments = ["-axo", "command="]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
        } catch {
            return false
        }
        let output = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return String(decoding: output, as: UTF8.self).lowercased().contains("needforspeedunbound")
    }
}
