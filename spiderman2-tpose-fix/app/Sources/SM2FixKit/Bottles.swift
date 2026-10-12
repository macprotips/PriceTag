import Foundation

/// A Wine bottle (CrossOver) or prefix (Whisky) on this Mac.
public struct Bottle: Hashable, Identifiable {
    public enum Kind: String {
        case crossOver = "CrossOver"
        case whisky = "Whisky"
    }

    public let kind: Kind
    public let name: String
    public let path: URL

    public init(kind: Kind, name: String, path: URL) {
        self.kind = kind
        self.name = name
        self.path = path
    }

    public var id: String { path.path }
    public var displayName: String { "\(kind.rawValue) bottle “\(name)”" }
    public var userReg: URL { path.appendingPathComponent("user.reg") }

    /// Whether Steam is installed inside this bottle.
    public var hasSteam: Bool {
        let fm = FileManager.default
        return Self.steamFolders.contains {
            fm.fileExists(atPath: path.appendingPathComponent("drive_c/\($0)/steam.exe").path)
        }
    }

    static let steamFolders = ["Program Files (x86)/Steam", "Program Files/Steam"]

    /// Where Windows path `windowsPath` (like `K:\SteamLibrary`) is on the Mac, using the
    /// bottle's drive-letter links in `dosdevices`.
    public func macURL(forWindowsPath windowsPath: String) -> URL? {
        let chars = Array(windowsPath)
        guard chars.count >= 2, chars[1] == ":", chars[0].isLetter else { return nil }
        let letter = String(chars[0]).lowercased()
        let drive = path.appendingPathComponent("dosdevices/\(letter):").resolvingSymlinksInPath()
        guard FileManager.default.fileExists(atPath: drive.path) else { return nil }
        let rest = String(chars.dropFirst(2)).replacingOccurrences(of: "\\", with: "/")
        let parts = rest.split(separator: "/").map(String.init)
        return parts.reduce(drive) { $0.appendingPathComponent($1) }
    }

    /// The Steam library folders this bottle's Steam knows about, as Mac paths.
    public func steamLibraries() -> [URL] {
        var libraries: [URL] = []
        for folder in Self.steamFolders {
            let vdf = path.appendingPathComponent("drive_c/\(folder)/steamapps/libraryfolders.vdf")
            guard let text = try? String(contentsOf: vdf, encoding: .utf8) else { continue }
            for windowsPath in Self.libraryPaths(inVDF: text) {
                if let url = macURL(forWindowsPath: windowsPath), !libraries.contains(url) {
                    libraries.append(url)
                }
            }
        }
        return libraries
    }

    /// The `"path"` entries of a Steam `libraryfolders.vdf`, unescaped.
    public static func libraryPaths(inVDF text: String) -> [String] {
        var paths: [String] = []
        for line in text.split(whereSeparator: \.isNewline) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            guard trimmed.hasPrefix("\"path\"") else { continue }
            let rest = trimmed.dropFirst(6).trimmingCharacters(in: .whitespaces)
            guard rest.count >= 2, rest.hasPrefix("\""), rest.hasSuffix("\"") else { continue }
            let value = String(rest.dropFirst().dropLast()).replacingOccurrences(of: "\\\\", with: "\\")
            paths.append(value)
        }
        return paths
    }
}

public enum Bottles {
    /// Every CrossOver and Whisky bottle in the user's home folder.
    public static func all(in env: SystemEnvironment) -> [Bottle] {
        let roots: [(Bottle.Kind, String)] = [
            (.crossOver, "Library/Application Support/CrossOver/Bottles"),
            (.whisky, "Library/Containers/com.isaacmarovitz.Whisky/Bottles"),
        ]
        let fm = FileManager.default
        var bottles: [Bottle] = []
        for (kind, relative) in roots {
            let root = env.home.appendingPathComponent(relative)
            let entries = (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil)) ?? []
            for entry in entries.sorted(by: { $0.lastPathComponent.lowercased() < $1.lastPathComponent.lowercased() }) {
                var isDir: ObjCBool = false
                if fm.fileExists(atPath: entry.appendingPathComponent("drive_c").path, isDirectory: &isDir), isDir.boolValue {
                    bottles.append(Bottle(kind: kind, name: Self.displayName(of: entry, kind: kind), path: entry))
                }
            }
        }
        return bottles
    }

    /// CrossOver bottles are named by their folder; Whisky's folders are UUIDs with the
    /// real name in Metadata.plist.
    static func displayName(of bottle: URL, kind: Bottle.Kind) -> String {
        if kind == .whisky,
           let data = try? Data(contentsOf: bottle.appendingPathComponent("Metadata.plist")),
           let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
           let info = plist["info"] as? [String: Any], let name = info["name"] as? String, !name.isEmpty {
            return name
        }
        return bottle.lastPathComponent
    }

    /// Which bottle runs the game in `gameFolder`: the one it is inside, else the one whose
    /// Steam lists its library, else the only bottle with Steam. `candidates` are the
    /// bottles to offer when it can't be told.
    public static func bottle(for gameFolder: URL, among bottles: [Bottle]) -> (bottle: Bottle?, candidates: [Bottle]) {
        if let inside = bottles.first(where: { gameFolder.isInside($0.path) }) {
            return (inside, [inside])
        }
        let byLibrary = bottles.filter { bottle in
            bottle.steamLibraries().contains { gameFolder.isInside($0) }
        }
        if byLibrary.count == 1 { return (byLibrary[0], byLibrary) }
        if byLibrary.count > 1 { return (nil, byLibrary) }
        let withSteam = bottles.filter(\.hasSteam)
        if withSteam.count == 1 { return (withSteam[0], withSteam) }
        return (nil, withSteam.isEmpty ? bottles : withSteam)
    }
}
