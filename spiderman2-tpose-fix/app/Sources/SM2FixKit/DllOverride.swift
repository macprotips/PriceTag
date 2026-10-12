import Foundation

/// The per-game Wine setting `d3d12 = native,builtin` for Spider-Man2.exe, which makes Wine
/// load the fix's d3d12.dll from the game folder. Stored in the bottle's user.reg.
public enum DllOverride {
    /// The key as written in user.reg (backslashes doubled).
    static let fileKey = #"Software\\Wine\\AppDefaults\\Spider-Man2.exe\\DllOverrides"#
    /// The key as `wine reg` takes it.
    static let regKey = #"HKCU\Software\Wine\AppDefaults\Spider-Man2.exe\DllOverrides"#
    static let valueLine = #""d3d12"="native,builtin""#

    /// The line range of the key's section in user.reg (header line first), if present.
    static func section(in lines: [String]) -> Range<Int>? {
        let header = "[" + fileKey.lowercased() + "]"
        guard let start = lines.firstIndex(where: { $0.lowercased().hasPrefix(header) }) else { return nil }
        var end = start + 1
        while end < lines.count, !lines[end].hasPrefix("[") { end += 1 }
        return start..<end
    }

    static func isD3D12Line(_ line: String) -> Bool {
        line.lowercased().hasPrefix(#""d3d12"="#)
    }

    /// Whether user.reg already makes Spider-Man2.exe load a native d3d12.dll first.
    public static func isSet(inUserReg text: String) -> Bool {
        let lines = text.components(separatedBy: "\n")
        guard let range = section(in: lines) else { return false }
        return lines[range].contains { line in
            isD3D12Line(line) && line.lowercased().replacingOccurrences(of: " ", with: "").dropFirst(8).hasPrefix("\"native")
        }
    }

    /// user.reg with the override added (or corrected).
    public static func adding(toUserReg text: String, now: Date = Date()) -> String {
        var lines = text.components(separatedBy: "\n")
        if let range = section(in: lines) {
            if let existing = lines[range].firstIndex(where: isD3D12Line) {
                lines[existing] = valueLine
            } else {
                var insertAt = range.lowerBound + 1
                if insertAt < lines.count, lines[insertAt].hasPrefix("#time=") { insertAt += 1 }
                lines.insert(valueLine, at: insertAt)
            }
            return lines.joined(separator: "\n")
        }
        let unix = Int(now.timeIntervalSince1970)
        let fileTime = String(UInt64(unix + 11_644_473_600) * 10_000_000, radix: 16)
        var out = text
        if !out.hasSuffix("\n") { out += "\n" }
        if !out.hasSuffix("\n\n") { out += "\n" }
        out += "[\(fileKey)] \(unix)\n#time=\(fileTime)\n\(valueLine)\n"
        return out
    }

    /// user.reg with the override removed.
    public static func removing(fromUserReg text: String) -> String {
        var lines = text.components(separatedBy: "\n")
        guard let range = section(in: lines), let line = lines[range].firstIndex(where: isD3D12Line) else { return text }
        lines.remove(at: line)
        return lines.joined(separator: "\n")
    }

    public enum Outcome: Equatable {
        case alreadySet
        case setWithWine
        case setInFile
        case removed
        case notNeeded
        /// The bottle's Wine is running and Wine's own tool isn't available: quit CrossOver/Whisky first.
        case wineRunning
        case failed(String)
    }

    /// The bottle's command-line Wine and the extra arguments/environment to address the bottle.
    struct WineTool {
        var executable: URL
        var arguments: [String]
        var environment: [String: String]
    }

    static func wineTool(for bottle: Bottle, env: SystemEnvironment) -> WineTool? {
        switch bottle.kind {
        case .crossOver:
            guard let wine = env.crossOverWine() else { return nil }
            return WineTool(executable: wine, arguments: ["--bottle", bottle.name], environment: [:])
        case .whisky:
            guard let wine = env.whiskyWine() else { return nil }
            return WineTool(executable: wine, arguments: [], environment: ["WINEPREFIX": bottle.path.path])
        }
    }

    /// Makes sure the override is set in `bottle`. When Wine isn't running, user.reg is
    /// edited directly: instant, and it starts no Wine processes. When Wine is running it
    /// keeps the registry in memory (and would overwrite the file on exit), so the
    /// bottle's own `wine reg` is used, which is quick because Wine is already up.
    public static func ensure(in bottle: Bottle, env: SystemEnvironment) -> Outcome {
        let current = (try? String(contentsOf: bottle.userReg, encoding: .utf8)) ?? ""
        if isSet(inUserReg: current) { return .alreadySet }
        return change(bottle, env: env, wineArguments: ["add", regKey, "/v", "d3d12", "/t", "REG_SZ", "/d", "native,builtin", "/f"],
                      done: .setWithWine, inFile: .setInFile) { adding(toUserReg: $0) }
    }

    /// Removes the override from `bottle` (used when the fix is removed).
    public static func remove(from bottle: Bottle, env: SystemEnvironment) -> Outcome {
        let current = (try? String(contentsOf: bottle.userReg, encoding: .utf8)) ?? ""
        guard let range = section(in: current.components(separatedBy: "\n")),
              current.components(separatedBy: "\n")[range].contains(where: isD3D12Line) else { return .notNeeded }
        return change(bottle, env: env, wineArguments: ["delete", regKey, "/v", "d3d12", "/f"],
                      done: .removed, inFile: .removed) { removing(fromUserReg: $0) }
    }

    private static func change(_ bottle: Bottle, env: SystemEnvironment, wineArguments: [String],
                               done: Outcome, inFile: Outcome, edit: (String) -> String) -> Outcome {
        guard env.wineIsRunning() else {
            return editFile(of: bottle, env: env, edit) ?? inFile
        }
        guard let tool = wineTool(for: bottle, env: env) else { return .wineRunning }
        let result = env.runner.run(tool.executable, tool.arguments + ["reg"] + wineArguments,
                                    environment: tool.environment.merging(["WINEDEBUG": "-all"]) { a, _ in a }, timeout: 30)
        return result.status == 0 ? done : .wineRunning
    }

    /// Edits user.reg in place (keeping a backup); nil on success.
    static func editFile(of bottle: Bottle, env: SystemEnvironment, _ edit: (String) -> String) -> Outcome? {
        if env.wineIsRunning() { return .wineRunning }
        guard let text = try? String(contentsOf: bottle.userReg, encoding: .utf8) else {
            return .failed("Couldn't read \(bottle.userReg.path)")
        }
        let backup = bottle.path.appendingPathComponent("user.reg.sm2fix-backup")
        let fm = FileManager.default
        do {
            if !fm.fileExists(atPath: backup.path) {
                try fm.copyItem(at: bottle.userReg, to: backup)
            }
            try edit(text).write(to: bottle.userReg, atomically: true, encoding: .utf8)
            return nil
        } catch {
            return .failed(error.localizedDescription)
        }
    }
}
