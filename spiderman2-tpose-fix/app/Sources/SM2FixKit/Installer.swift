import Foundation
#if canImport(Darwin)
import Darwin
#endif

/// What the app carries: the d3d12.dll shim, its settings template and Microsoft's shader
/// tools. The rewritten shaders are built from the user's own game, so none ship with it.
public struct Payload {
    public let dll: URL
    public let ini: URL
    /// Folder with dxc, dxa and dxv.
    public let tools: URL

    public enum LoadError: Error, LocalizedError {
        case missing(String)
        public var errorDescription: String? {
            switch self {
            case .missing(let name): return "The app is missing its \(name). Download it again."
            }
        }
    }

    /// Loads the payload from a folder holding d3d12.dll and sm2fix.ini, with the shader
    /// tools in `tools`.
    public init(directory: URL, tools: URL) throws {
        let fm = FileManager.default
        let dllFile = directory.appendingPathComponent("d3d12.dll")
        let iniFile = directory.appendingPathComponent("sm2fix.ini")
        for file in [dllFile, iniFile] where !fm.fileExists(atPath: file.path) {
            throw LoadError.missing(file.lastPathComponent)
        }
        guard ShaderBuilder(tools: tools).toolsPresent else { throw LoadError.missing("shader tools") }
        dll = dllFile
        ini = iniFile
        self.tools = tools
    }
}

/// Whether the fix is in a game folder.
public enum FixStatus: Equatable {
    case notInstalled
    case installed
    /// This fix, but an older build of it.
    case updateAvailable
    /// One of the earlier sm2fix test builds (from the troubleshooting menu).
    case testBuildInstalled
}

public enum InstallError: Error, LocalizedError, Equatable {
    case notAGameFolder
    case gameRunning
    case readOnly(String)
    case copyFailed(String)
    case shaders(ShaderBuildError)

    public var errorDescription: String? {
        switch self {
        case .notAGameFolder:
            return "That folder doesn't contain Spider-Man2.exe."
        case .gameRunning:
            return "Spider-Man 2 is still running. Quit it (or quit CrossOver completely), then try again."
        case .readOnly(let path):
            return "Can't write to \(path). If the game is on an external drive formatted as NTFS, macOS can only read it."
        case .copyFailed(let reason):
            return "Couldn't copy the fix into the game folder: \(reason)"
        case .shaders(let error):
            return error.errorDescription
        }
    }
}

/// What an install or removal did, for showing to the user.
public struct InstallReport: Equatable {
    public var override: DllOverride.Outcome?
    public var notes: [String] = []

    public init(override: DllOverride.Outcome? = nil, notes: [String] = []) {
        self.override = override
        self.notes = notes
    }
}

public struct Installer {
    public let payload: Payload
    public let env: SystemEnvironment
    public let shaders: [PoolShader]

    public init(payload: Payload, env: SystemEnvironment, shaders: [PoolShader] = PoolShader.spiderMan2) {
        self.payload = payload
        self.env = env
        self.shaders = shaders
    }

    static let managedFiles = ["d3d12.dll", "sm2fix.ini", "sm2fix.log", "sm2fix-skinning.dxil"]
    static let managedFolders = ["sm2fix-dumps", "sm2fix-capture"]
    static let backupSuffix = ".sm2fix-backup"

    static func isShim(_ file: URL) -> Bool {
        guard let data = try? Data(contentsOf: file, options: .mappedIfSafe) else { return false }
        return data.range(of: Data("sm2fix".utf8)) != nil
    }

    static func iniValue(_ key: String, in text: String) -> String? {
        for line in text.components(separatedBy: .newlines) {
            let parts = line.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            if parts.count == 2, parts[0] == key { return parts[1] }
        }
        return nil
    }

    public func status(of folder: URL) -> FixStatus {
        let dll = folder.appendingPathComponent("d3d12.dll")
        guard Self.isShim(dll) else { return .notInstalled }
        let ini = (try? String(contentsOf: folder.appendingPathComponent("sm2fix.ini"), encoding: .utf8)) ?? ""
        guard Self.iniValue("pool_fix", in: ini) == "1" else { return .testBuildInstalled }
        let fm = FileManager.default
        let sameDll = fm.contentsEqual(atPath: dll.path, andPath: payload.dll.path)
        let sameShaders = shaders.allSatisfy { shader in
            guard let data = try? Data(contentsOf: folder.appendingPathComponent(shader.fileName)) else { return false }
            return shader.resultHash.map { shaderHash(data) == $0 } ?? true
        }
        let cleanSettings = Self.iniValue("mode", in: ini) == "off"
        return sameDll && sameShaders && cleanSettings ? .installed : .updateAvailable
    }

    /// The settings file: the shipped template with only the fix switched on.
    func settings() throws -> String {
        let template = try String(contentsOf: payload.ini, encoding: .utf8)
        var lines = template.components(separatedBy: "\n")
        for (i, line) in lines.enumerated() {
            if line.hasPrefix("mode =") || line.hasPrefix("mode=") { lines[i] = "mode = off" }
            if line.hasPrefix("pool_fix =") || line.hasPrefix("pool_fix=") { lines[i] = "pool_fix = 1" }
        }
        var text = lines.joined(separator: "\n")
        if Self.iniValue("pool_fix", in: text) != "1" { text += "\npool_fix = 1\n" }
        return text
    }

    func checkWritable(_ folder: URL) throws {
        guard GameFinder.hasExe(folder) else { throw InstallError.notAGameFolder }
        if env.gameIsRunning() { throw InstallError.gameRunning }
        let probe = folder.appendingPathComponent(".sm2fix-write-test")
        guard FileManager.default.createFile(atPath: probe.path, contents: Data()) else {
            throw InstallError.readOnly(folder.path)
        }
        try? FileManager.default.removeItem(at: probe)
    }

    static func replace(_ destination: URL, with source: URL) throws {
        let fm = FileManager.default
        if fm.fileExists(atPath: destination.path) || (try? destination.checkResourceIsReachable()) == true {
            try fm.removeItem(at: destination)
        }
        try fm.copyItem(at: source, to: destination)
        #if canImport(Darwin)
        removexattr(destination.path, "com.apple.quarantine", 0)
        #endif
    }

    /// Installs (or updates) the fix in `game.folder` and sets the DLL override in its bottle.
    /// The shaders are built from the game's own Spider-Man2.exe first; nothing in the game
    /// folder changes unless that works.
    public func install(_ game: GameInstall) throws -> InstallReport {
        let folder = game.folder
        try checkWritable(folder)
        let fm = FileManager.default
        var report = InstallReport()
        let built: [String: Data]
        do {
            built = try ShaderBuilder(tools: payload.tools)
                .build(shaders, fromGame: folder.appendingPathComponent(GameFinder.exeName))
        } catch let error as ShaderBuildError {
            throw InstallError.shaders(error)
        }

        let dll = folder.appendingPathComponent("d3d12.dll")
        let backup = folder.appendingPathComponent("d3d12.dll" + Self.backupSuffix)
        if fm.fileExists(atPath: dll.path), !Self.isShim(dll), !fm.fileExists(atPath: backup.path) {
            do {
                try fm.moveItem(at: dll, to: backup)
                report.notes.append("Your game folder already had a d3d12.dll; it was kept as d3d12.dll\(Self.backupSuffix).")
            } catch {
                throw InstallError.copyFailed(error.localizedDescription)
            }
        }
        do {
            // Leftovers from the troubleshooting tests.
            for name in Self.managedFiles + Self.managedFolders where name != "d3d12.dll" {
                try? fm.removeItem(at: folder.appendingPathComponent(name))
            }
            for old in (try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
            where old.lastPathComponent.hasPrefix("sm2fix-pool-") && old.pathExtension == "dxil" {
                try? fm.removeItem(at: old)
            }
            try Self.replace(dll, with: payload.dll)
            for (name, data) in built.sorted(by: { $0.key < $1.key }) {
                try data.write(to: folder.appendingPathComponent(name), options: .atomic)
            }
            try settings().write(to: folder.appendingPathComponent("sm2fix.ini"), atomically: true, encoding: .utf8)
        } catch {
            throw InstallError.copyFailed(error.localizedDescription)
        }

        // DirectStorage switched off by an earlier test: switch it back on.
        for name in ["dstorage.dll", "dstoragecore.dll"] {
            let off = folder.appendingPathComponent(name + ".sm2fix-off")
            let on = folder.appendingPathComponent(name)
            if fm.fileExists(atPath: off.path), !fm.fileExists(atPath: on.path) {
                try? fm.moveItem(at: off, to: on)
            }
        }

        if let bottle = game.bottle {
            report.override = DllOverride.ensure(in: bottle, env: env)
        }
        return report
    }

    /// Removes the fix from `game.folder`, puts back a d3d12.dll it replaced, and removes
    /// the DLL override when nothing needs it any more.
    public func uninstall(_ game: GameInstall) throws -> InstallReport {
        let folder = game.folder
        try checkWritable(folder)
        let fm = FileManager.default
        var report = InstallReport()
        let dll = folder.appendingPathComponent("d3d12.dll")
        do {
            if fm.fileExists(atPath: dll.path), Self.isShim(dll) { try fm.removeItem(at: dll) }
            for name in Self.managedFiles + Self.managedFolders where name != "d3d12.dll" {
                try? fm.removeItem(at: folder.appendingPathComponent(name))
            }
            for old in (try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
            where old.lastPathComponent.hasPrefix("sm2fix-pool-") && old.pathExtension == "dxil" {
                try fm.removeItem(at: old)
            }
            let backup = folder.appendingPathComponent("d3d12.dll" + Self.backupSuffix)
            if fm.fileExists(atPath: backup.path), !fm.fileExists(atPath: dll.path) {
                try fm.moveItem(at: backup, to: dll)
                report.notes.append("Put back the d3d12.dll your game folder had before.")
            }
        } catch {
            throw InstallError.copyFailed(error.localizedDescription)
        }
        if let bottle = game.bottle, !fm.fileExists(atPath: dll.path) {
            report.override = DllOverride.remove(from: bottle, env: env)
        }
        return report
    }
}
