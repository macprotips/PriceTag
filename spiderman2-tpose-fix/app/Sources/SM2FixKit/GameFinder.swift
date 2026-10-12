import Foundation

/// A copy of the game and the bottle that runs it.
public struct GameInstall: Equatable {
    public var folder: URL
    /// The bottle that runs it, when it could be told.
    public var bottle: Bottle?
    /// Bottles to choose from when `bottle` is nil (or to change it).
    public var candidates: [Bottle]

    public init(folder: URL, bottle: Bottle?, candidates: [Bottle]) {
        self.folder = folder
        self.bottle = bottle
        self.candidates = candidates
    }
}

public enum GameFinder {
    public static let exeName = "Spider-Man2.exe"
    public static let folderName = "Marvel's Spider-Man 2"
    static let commonSubpath = "steamapps/common/" + folderName

    static func hasExe(_ folder: URL) -> Bool {
        FileManager.default.fileExists(atPath: folder.appendingPathComponent(exeName).path)
    }

    /// The game folder for something the user dropped or picked: the game folder itself,
    /// Spider-Man2.exe, or a folder above it (a Steam library, a drive, `steamapps`...).
    public static func gameFolder(from url: URL) -> URL? {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return nil }
        if !isDir.boolValue {
            let parent = url.deletingLastPathComponent()
            return url.lastPathComponent.caseInsensitiveCompare(exeName) == .orderedSame && hasExe(parent) ? parent : nil
        }
        if hasExe(url) { return url }
        for sub in [commonSubpath, "common/" + folderName, folderName, "SteamLibrary/" + commonSubpath] {
            let candidate = url.appendingPathComponent(sub)
            if hasExe(candidate) { return candidate }
        }
        return search(below: url, depth: 6)
    }

    /// A bounded search for Spider-Man2.exe under `root`, skipping Windows system folders.
    static func search(below root: URL, depth: Int) -> URL? {
        let fm = FileManager.default
        var queue: [(URL, Int)] = [(root, 0)]
        var visited = 0
        let skip: Set<String> = ["windows", "programdata", "users", ".trash", "library", "system", "private", "dosdevices"]
        while !queue.isEmpty, visited < 5000 {
            let (dir, level) = queue.removeFirst()
            visited += 1
            if hasExe(dir) { return dir }
            guard level < depth else { continue }
            let children = (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey],
                                                        options: [.skipsHiddenFiles, .skipsPackageDescendants])) ?? []
            for child in children.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
                let values = try? child.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
                guard values?.isDirectory == true, values?.isSymbolicLink != true,
                      !skip.contains(child.lastPathComponent.lowercased()) else { continue }
                queue.append((child, level + 1))
            }
        }
        return nil
    }

    /// The install for a game folder: which bottle runs it.
    public static func install(at folder: URL, env: SystemEnvironment) -> GameInstall {
        let found = Bottles.bottle(for: folder, among: Bottles.all(in: env))
        return GameInstall(folder: folder, bottle: found.bottle, candidates: found.candidates)
    }

    /// Every copy of the game on this Mac: inside bottles, in the Steam libraries they
    /// know about, and on external drives.
    public static func findAll(env: SystemEnvironment) -> [GameInstall] {
        let bottles = Bottles.all(in: env)
        var folders: [URL] = []
        func add(_ folder: URL) {
            if hasExe(folder), !folders.contains(where: { $0.canonicalPath == folder.canonicalPath }) {
                folders.append(folder)
            }
        }
        for bottle in bottles {
            for steam in Bottle.steamFolders {
                add(bottle.path.appendingPathComponent("drive_c/\(steam)/\(commonSubpath)"))
            }
            for library in bottle.steamLibraries() {
                add(library.appendingPathComponent(commonSubpath))
            }
        }
        let fm = FileManager.default
        let drives = (try? fm.contentsOfDirectory(at: env.volumes, includingPropertiesForKeys: nil)) ?? []
        for drive in drives {
            add(drive.appendingPathComponent(commonSubpath))
            let level1 = (try? fm.contentsOfDirectory(at: drive, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? []
            for dir in level1 {
                add(dir.appendingPathComponent(commonSubpath))
                add(dir.appendingPathComponent("Steam/" + commonSubpath))
            }
        }
        return folders.map { folder in
            let found = Bottles.bottle(for: folder, among: bottles)
            return GameInstall(folder: folder, bottle: found.bottle, candidates: found.candidates)
        }
    }
}
