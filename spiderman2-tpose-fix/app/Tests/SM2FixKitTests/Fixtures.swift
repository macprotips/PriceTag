import Foundation
import XCTest
@testable import SM2FixKit

/// Records commands and answers them: `ps` lists `processes`, wine reg exits with `wineStatus`.
final class FakeRunner: CommandRunner {
    var processes: [String] = []
    var wineStatus: Int32 = 0
    var calls: [(String, [String], [String: String])] = []

    func run(_ executable: URL, _ arguments: [String], environment: [String: String], timeout: TimeInterval) -> CommandResult {
        calls.append((executable.path, arguments, environment))
        if executable.lastPathComponent == "ps" {
            return CommandResult(status: 0, output: processes.joined(separator: "\n"))
        }
        return CommandResult(status: wineStatus, output: "")
    }

    var wineCalls: [[String]] { calls.filter { !$0.0.hasSuffix("/ps") }.map { $0.1 } }
}

/// A fake Mac: a home folder with bottles, a /Volumes folder and an Applications folder.
final class FakeMac {
    let root: URL
    let home: URL
    let volumes: URL
    let apps: URL
    let runner = FakeRunner()
    let fm = FileManager.default

    init() throws {
        root = fm.temporaryDirectory.appendingPathComponent("sm2fix-tests-\(UUID().uuidString)")
        home = root.appendingPathComponent("home")
        volumes = root.appendingPathComponent("Volumes")
        apps = root.appendingPathComponent("Applications")
        for dir in [home, volumes, apps] {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        }
    }

    deinit {
        try? fm.removeItem(at: root)
    }

    var env: SystemEnvironment {
        SystemEnvironment(home: home, volumes: volumes, applicationFolders: [apps], runner: runner)
    }

    @discardableResult
    func bottle(_ name: String, kind: Bottle.Kind = .crossOver, steam: Bool = true, userReg: String = "WINE REGISTRY Version 2\n") throws -> URL {
        let base = kind == .crossOver ? "Library/Application Support/CrossOver/Bottles"
                                      : "Library/Containers/com.isaacmarovitz.Whisky/Bottles"
        let bottle = home.appendingPathComponent(base).appendingPathComponent(name)
        try fm.createDirectory(at: bottle.appendingPathComponent("drive_c/windows/system32"), withIntermediateDirectories: true)
        try fm.createDirectory(at: bottle.appendingPathComponent("dosdevices"), withIntermediateDirectories: true)
        try fm.createSymbolicLink(atPath: bottle.appendingPathComponent("dosdevices/c:").path, withDestinationPath: "../drive_c")
        if steam {
            let steamDir = bottle.appendingPathComponent("drive_c/Program Files (x86)/Steam")
            try fm.createDirectory(at: steamDir.appendingPathComponent("steamapps"), withIntermediateDirectories: true)
            try Data().write(to: steamDir.appendingPathComponent("steam.exe"))
        }
        try userReg.write(to: bottle.appendingPathComponent("user.reg"), atomically: true, encoding: .utf8)
        return bottle
    }

    /// A drive letter in `bottle` pointing at `target`.
    func drive(_ letter: String, in bottle: URL, to target: URL) throws {
        try fm.createSymbolicLink(atPath: bottle.appendingPathComponent("dosdevices/\(letter):").path, withDestinationPath: target.path)
    }

    func libraryVDF(in bottle: URL, paths: [String]) throws {
        var text = "\"libraryfolders\"\n{\n"
        for (i, path) in paths.enumerated() {
            let escaped = path.replacingOccurrences(of: "\\", with: "\\\\")
            text += "\t\"\(i)\"\n\t{\n\t\t\"path\"\t\t\"\(escaped)\"\n\t\t\"label\"\t\t\"\"\n\t}\n"
        }
        text += "}\n"
        try text.write(to: bottle.appendingPathComponent("drive_c/Program Files (x86)/Steam/steamapps/libraryfolders.vdf"),
                       atomically: true, encoding: .utf8)
    }

    @discardableResult
    func game(in folder: URL) throws -> URL {
        let game = folder.appendingPathComponent("steamapps/common/Marvel's Spider-Man 2")
        try fm.createDirectory(at: game, withIntermediateDirectories: true)
        try Data("MZ fake game".utf8).write(to: game.appendingPathComponent("Spider-Man2.exe"))
        try Data("MZ dstorage".utf8).write(to: game.appendingPathComponent("dstorage.dll"))
        return game
    }

    func crossOverApp() throws -> URL {
        let bin = apps.appendingPathComponent("CrossOver.app/Contents/SharedSupport/CrossOver/bin")
        try fm.createDirectory(at: bin, withIntermediateDirectories: true)
        let wine = bin.appendingPathComponent("wine")
        try "#!/bin/sh\nexit 0\n".write(to: wine, atomically: true, encoding: .utf8)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: wine.path)
        return wine
    }

    /// The real payload from the repository, copied into a temp folder.
    func payload() throws -> Payload {
        let repo = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let dir = root.appendingPathComponent("payload")
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        try fm.copyItem(at: repo.appendingPathComponent("prebuilt/d3d12.dll"), to: dir.appendingPathComponent("d3d12.dll"))
        try fm.copyItem(at: repo.appendingPathComponent("shim/sm2fix.ini"), to: dir.appendingPathComponent("sm2fix.ini"))
        let shaders = try fm.contentsOfDirectory(at: repo.appendingPathComponent("shader-fix"), includingPropertiesForKeys: nil)
            .filter { $0.lastPathComponent.hasPrefix("sm2fix-pool-") }
        XCTAssertEqual(shaders.count, 2)
        for shader in shaders {
            try fm.copyItem(at: shader, to: dir.appendingPathComponent(shader.lastPathComponent))
        }
        return try Payload(directory: dir)
    }
}
