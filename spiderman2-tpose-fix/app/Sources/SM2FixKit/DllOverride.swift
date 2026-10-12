import Foundation

/// The per-game Wine setting `d3d12 = native,builtin` for Spider-Man2.exe, which makes Wine
/// load the fix's d3d12.dll from the game folder. Stored in the bottle's user.reg, which
/// the app edits directly (it never runs Wine).
public enum DllOverride {
    /// The key as written in user.reg (backslashes doubled).
    static let fileKey = #"Software\\Wine\\AppDefaults\\Spider-Man2.exe\\DllOverrides"#
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
        case setInFile
        case removed
        case notNeeded
        /// Wine is running, and it would overwrite the change when it exits: quit CrossOver first.
        case wineRunning
        case failed(String)
    }

    /// Makes sure the override is set in `bottle`, by editing its user.reg. The app never
    /// starts Wine: while Wine runs it keeps the registry in memory and would overwrite
    /// the file when it exits, so then the user is asked to quit CrossOver first.
    public static func ensure(in bottle: Bottle, env: SystemEnvironment) -> Outcome {
        let current = (try? String(contentsOf: bottle.userReg, encoding: .utf8)) ?? ""
        if isSet(inUserReg: current) { return .alreadySet }
        return editFile(of: bottle, env: env) { adding(toUserReg: $0) } ?? .setInFile
    }

    /// Removes the override from `bottle` (used when the fix is removed).
    public static func remove(from bottle: Bottle, env: SystemEnvironment) -> Outcome {
        let current = (try? String(contentsOf: bottle.userReg, encoding: .utf8)) ?? ""
        guard let range = section(in: current.components(separatedBy: "\n")),
              current.components(separatedBy: "\n")[range].contains(where: isD3D12Line) else { return .notNeeded }
        return editFile(of: bottle, env: env) { removing(fromUserReg: $0) } ?? .removed
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
