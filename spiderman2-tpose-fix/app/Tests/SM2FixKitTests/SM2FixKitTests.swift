import Foundation
import XCTest
@testable import SM2FixKit

final class BottleTests: XCTestCase {
    func testLibraryPathsFromVDF() {
        let vdf = """
        "libraryfolders"
        {
        \t"0"
        \t{
        \t\t"path"\t\t"C:\\\\Program Files (x86)\\\\Steam"
        \t}
        \t"1"
        \t{
        \t\t"path"\t\t"K:\\\\SteamLibrary"
        \t\t"label"\t\t""
        \t}
        }
        """
        XCTAssertEqual(Bottle.libraryPaths(inVDF: vdf), [#"C:\Program Files (x86)\Steam"#, #"K:\SteamLibrary"#])
    }

    func testFindsBottlesWithDriveC() throws {
        let mac = try FakeMac()
        try mac.bottle("Steam")
        try mac.bottle("Games", kind: .whisky, steam: false)
        try mac.fm.createDirectory(at: mac.home.appendingPathComponent("Library/Application Support/CrossOver/Bottles/empty"),
                                   withIntermediateDirectories: true)
        let bottles = Bottles.all(in: mac.env)
        XCTAssertEqual(bottles.map(\.name), ["Steam", "Games"])
        XCTAssertEqual(bottles.map(\.kind), [.crossOver, .whisky])
        XCTAssertTrue(bottles[0].hasSteam)
        XCTAssertFalse(bottles[1].hasSteam)
    }

    func testDriveLettersMapToMacFolders() throws {
        let mac = try FakeMac()
        let bottle = try mac.bottle("Steam")
        let drive = mac.volumes.appendingPathComponent("MPT")
        try mac.fm.createDirectory(at: drive.appendingPathComponent("SteamLibrary"), withIntermediateDirectories: true)
        try mac.drive("k", in: bottle, to: drive)
        let b = Bottles.all(in: mac.env)[0]
        XCTAssertEqual(b.macURL(forWindowsPath: #"K:\SteamLibrary"#)?.canonicalPath,
                       drive.appendingPathComponent("SteamLibrary").canonicalPath)
        XCTAssertEqual(b.macURL(forWindowsPath: #"C:\Program Files (x86)"#)?.canonicalPath,
                       bottle.appendingPathComponent("drive_c/Program Files (x86)").canonicalPath)
        XCTAssertNil(b.macURL(forWindowsPath: #"Q:\nothing"#))
        XCTAssertNil(b.macURL(forWindowsPath: "relative"))
    }

    func testBottleForGameInsideBottle() throws {
        let mac = try FakeMac()
        try mac.bottle("Other")
        let bottle = try mac.bottle("Steam")
        let game = try mac.game(in: bottle.appendingPathComponent("drive_c/Program Files (x86)/Steam"))
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(install.bottle?.name, "Steam")
    }

    func testBottleForGameOnExternalDriveViaSteamLibrary() throws {
        let mac = try FakeMac()
        let other = try mac.bottle("Other")
        let bottle = try mac.bottle("Steam")
        let drive = mac.volumes.appendingPathComponent("MPT")
        let game = try mac.game(in: drive.appendingPathComponent("SteamLibrary"))
        try mac.drive("k", in: bottle, to: drive)
        try mac.libraryVDF(in: bottle, paths: [#"C:\Program Files (x86)\Steam"#, #"K:\SteamLibrary"#])
        try mac.libraryVDF(in: other, paths: [#"C:\Program Files (x86)\Steam"#])
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(install.bottle?.name, "Steam")
    }

    func testOnlyBottleWithSteamIsPicked() throws {
        let mac = try FakeMac()
        try mac.bottle("Office", steam: false)
        try mac.bottle("Steam")
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        XCTAssertEqual(GameFinder.install(at: game, env: mac.env).bottle?.name, "Steam")
    }

    func testAmbiguousBottleOffersCandidates() throws {
        let mac = try FakeMac()
        try mac.bottle("A")
        try mac.bottle("B")
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertNil(install.bottle)
        XCTAssertEqual(install.candidates.map(\.name), ["A", "B"])
    }
}

final class GameFinderTests: XCTestCase {
    func testGameFolderFromWhatTheUserDrops() throws {
        let mac = try FakeMac()
        let library = mac.volumes.appendingPathComponent("MPT/SteamLibrary")
        let game = try mac.game(in: library)
        let exe = game.appendingPathComponent("Spider-Man2.exe")
        XCTAssertEqual(GameFinder.gameFolder(from: game)?.canonicalPath, game.canonicalPath)
        XCTAssertEqual(GameFinder.gameFolder(from: exe)?.canonicalPath, game.canonicalPath)
        XCTAssertEqual(GameFinder.gameFolder(from: library)?.canonicalPath, game.canonicalPath)
        XCTAssertEqual(GameFinder.gameFolder(from: mac.volumes.appendingPathComponent("MPT"))?.canonicalPath, game.canonicalPath)
        XCTAssertNil(GameFinder.gameFolder(from: game.appendingPathComponent("dstorage.dll")))
        XCTAssertNil(GameFinder.gameFolder(from: mac.home))
        XCTAssertNil(GameFinder.gameFolder(from: mac.root.appendingPathComponent("missing")))
    }

    func testFindAll() throws {
        let mac = try FakeMac()
        let bottle = try mac.bottle("Steam")
        let drive = mac.volumes.appendingPathComponent("MPT")
        let external = try mac.game(in: drive.appendingPathComponent("SteamLibrary"))
        try mac.drive("k", in: bottle, to: drive)
        try mac.libraryVDF(in: bottle, paths: [#"K:\SteamLibrary"#])
        let inside = try mac.game(in: bottle.appendingPathComponent("drive_c/Program Files (x86)/Steam"))
        let found = GameFinder.findAll(env: mac.env)
        XCTAssertEqual(Set(found.map(\.folder.canonicalPath)), [external.canonicalPath, inside.canonicalPath])
        XCTAssertTrue(found.allSatisfy { $0.bottle?.name == "Steam" })
    }
}

final class DllOverrideTests: XCTestCase {
    let reg = "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n[Software\\\\Wine] 1700000000\n#time=1d9\n\"Version\"=\"win10\"\n"

    func testAddAndRemoveRoundTrip() {
        XCTAssertFalse(DllOverride.isSet(inUserReg: reg))
        let added = DllOverride.adding(toUserReg: reg, now: Date(timeIntervalSince1970: 1_760_000_000))
        XCTAssertTrue(DllOverride.isSet(inUserReg: added))
        XCTAssertTrue(added.hasPrefix(reg))
        XCTAssertTrue(added.contains("[Software\\\\Wine\\\\AppDefaults\\\\Spider-Man2.exe\\\\DllOverrides] 1760000000\n#time="))
        XCTAssertTrue(added.contains("\"Version\"=\"win10\""))
        XCTAssertEqual(DllOverride.adding(toUserReg: added), added)
        let removed = DllOverride.removing(fromUserReg: added)
        XCTAssertFalse(DllOverride.isSet(inUserReg: removed))
        XCTAssertTrue(removed.contains("\"Version\"=\"win10\""))
    }

    func testFixesAWrongValueInAnExistingSection() {
        let text = reg + "\n[Software\\\\Wine\\\\AppDefaults\\\\Spider-Man2.exe\\\\DllOverrides] 1\n#time=1\n\"d3d12\"=\"builtin\"\n\"dxgi\"=\"native\"\n"
        XCTAssertFalse(DllOverride.isSet(inUserReg: text))
        let fixed = DllOverride.adding(toUserReg: text)
        XCTAssertTrue(DllOverride.isSet(inUserReg: fixed))
        XCTAssertTrue(fixed.contains("\"dxgi\"=\"native\""))
        XCTAssertEqual(fixed.components(separatedBy: "\"d3d12\"").count, 2)
    }

    /// While Wine runs, the change goes through the bottle's own `wine reg`.
    func testUsesCrossOverWineWhileWineRuns() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam", userReg: reg)
        let wine = try mac.crossOverApp()
        mac.runner.processes = ["/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver"]
        let bottle = Bottles.all(in: mac.env)[0]
        XCTAssertEqual(DllOverride.ensure(in: bottle, env: mac.env), .setWithWine)
        XCTAssertEqual(mac.runner.calls.last?.0, wine.path)
        XCTAssertEqual(mac.runner.wineCalls.last, ["--bottle", "Steam", "reg", "add",
                                                   #"HKCU\Software\Wine\AppDefaults\Spider-Man2.exe\DllOverrides"#,
                                                   "/v", "d3d12", "/t", "REG_SZ", "/d", "native,builtin", "/f"])
        XCTAssertEqual(try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8), reg)
    }

    /// With CrossOver installed but not running, no Wine process is started at all.
    func testStartsNoWineWhenWineIsNotRunning() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam", userReg: reg)
        _ = try mac.crossOverApp()
        let bottle = Bottles.all(in: mac.env)[0]
        XCTAssertEqual(DllOverride.ensure(in: bottle, env: mac.env), .setInFile)
        XCTAssertEqual(DllOverride.remove(from: bottle, env: mac.env), .removed)
        XCTAssertEqual(mac.runner.wineCalls, [])
        XCTAssertFalse(DllOverride.isSet(inUserReg: try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8)))
    }

    func testEditsUserRegWithoutWine() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam", userReg: reg)
        let bottle = Bottles.all(in: mac.env)[0]
        XCTAssertEqual(DllOverride.ensure(in: bottle, env: mac.env), .setInFile)
        let text = try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8)
        XCTAssertTrue(DllOverride.isSet(inUserReg: text))
        XCTAssertEqual(try String(contentsOf: path.appendingPathComponent("user.reg.sm2fix-backup"), encoding: .utf8), reg)
        XCTAssertEqual(DllOverride.ensure(in: bottle, env: mac.env), .alreadySet)
        XCTAssertEqual(DllOverride.remove(from: bottle, env: mac.env), .removed)
        XCTAssertFalse(DllOverride.isSet(inUserReg: try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8)))
        XCTAssertEqual(DllOverride.remove(from: bottle, env: mac.env), .notNeeded)
    }

    func testWontEditUserRegWhileWineRuns() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam", userReg: reg)
        mac.runner.processes = ["/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver"]
        XCTAssertEqual(DllOverride.ensure(in: Bottles.all(in: mac.env)[0], env: mac.env), .wineRunning)
        XCTAssertEqual(try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8), reg)
    }

    /// Wine running but its `reg` failing: never edit the file under a running Wine.
    func testAsksToQuitWineWhenWineRegFails() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam", userReg: reg)
        _ = try mac.crossOverApp()
        mac.runner.processes = ["/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver"]
        mac.runner.wineStatus = 1
        XCTAssertEqual(DllOverride.ensure(in: Bottles.all(in: mac.env)[0], env: mac.env), .wineRunning)
        XCTAssertEqual(try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8), reg)
    }
}

final class InstallerTests: XCTestCase {
    func testInstallStatusAndUninstall() throws {
        let mac = try FakeMac()
        let path = try mac.bottle("Steam")
        let game = try mac.game(in: path.appendingPathComponent("drive_c/Program Files (x86)/Steam"))
        let installer = try mac.installer()
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(installer.status(of: game), .notInstalled)

        // leftovers from the troubleshooting tests
        try Data("old".utf8).write(to: game.appendingPathComponent("sm2fix-skinning.dxil"))
        try mac.fm.createDirectory(at: game.appendingPathComponent("sm2fix-capture"), withIntermediateDirectories: true)
        try mac.fm.moveItem(at: game.appendingPathComponent("dstorage.dll"), to: game.appendingPathComponent("dstorage.dll.sm2fix-off"))

        let report = try installer.install(install)
        XCTAssertEqual(report.override, .setInFile)
        XCTAssertEqual(installer.status(of: game), .installed)
        let shaderFiles = installer.shaders.map(\.fileName)
        for name in ["d3d12.dll", "sm2fix.ini", "dstorage.dll"] + shaderFiles {
            XCTAssertTrue(mac.fm.fileExists(atPath: game.appendingPathComponent(name).path), name)
        }
        for name in ["sm2fix-skinning.dxil", "sm2fix-capture", "dstorage.dll.sm2fix-off"] {
            XCTAssertFalse(mac.fm.fileExists(atPath: game.appendingPathComponent(name).path), name)
        }
        let ini = try String(contentsOf: game.appendingPathComponent("sm2fix.ini"), encoding: .utf8)
        XCTAssertEqual(Installer.iniValue("mode", in: ini), "off")
        XCTAssertEqual(Installer.iniValue("pool_fix", in: ini), "1")
        XCTAssertEqual(Installer.iniValue("capture", in: ini), "0")
        XCTAssertEqual(Installer.iniValue("replace_skinning", in: ini), "0")
        XCTAssertTrue(DllOverride.isSet(inUserReg: try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8)))

        // a changed shader -> update available; installing again fixes it
        try Data("DXBC stale".utf8).write(to: game.appendingPathComponent(shaderFiles[0]))
        XCTAssertEqual(installer.status(of: game), .updateAvailable)
        XCTAssertEqual(try installer.install(install).override, .alreadySet)
        XCTAssertEqual(installer.status(of: game), .installed)

        let removed = try installer.uninstall(install)
        XCTAssertEqual(removed.override, .removed)
        XCTAssertEqual(installer.status(of: game), .notInstalled)
        let left = try mac.fm.contentsOfDirectory(atPath: game.path).sorted()
        XCTAssertEqual(left, ["Spider-Man2.exe", "dstorage.dll"])
        XCTAssertFalse(DllOverride.isSet(inUserReg: try String(contentsOf: path.appendingPathComponent("user.reg"), encoding: .utf8)))
    }

    func testKeepsAndRestoresAnotherD3D12() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        let installer = try mac.installer()
        let install = GameInstall(folder: game, bottle: nil, candidates: [])
        try Data("someone else's d3d12".utf8).write(to: game.appendingPathComponent("d3d12.dll"))
        let report = try installer.install(install)
        XCTAssertNil(report.override)
        XCTAssertEqual(report.notes.count, 1)
        XCTAssertEqual(installer.status(of: game), .installed)
        _ = try installer.uninstall(install)
        XCTAssertEqual(try Data(contentsOf: game.appendingPathComponent("d3d12.dll")), Data("someone else's d3d12".utf8))
        XCTAssertFalse(mac.fm.fileExists(atPath: game.appendingPathComponent("d3d12.dll.sm2fix-backup").path))
    }

    func testRecognisesTestBuilds() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        let installer = try mac.installer()
        try mac.fm.copyItem(at: installer.payload.dll, to: game.appendingPathComponent("d3d12.dll"))
        try "mode = all\npool_fix = 0\n".write(to: game.appendingPathComponent("sm2fix.ini"), atomically: true, encoding: .utf8)
        XCTAssertEqual(installer.status(of: game), .testBuildInstalled)
    }

    func testRefusesWhileTheGameRuns() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        mac.runner.processes = [#"Z:\Volumes\Ext\steamapps\common\Marvel's Spider-Man 2\Spider-Man2.exe"#]
        let installer = try mac.installer()
        XCTAssertThrowsError(try installer.install(GameInstall(folder: game, bottle: nil, candidates: []))) {
            XCTAssertEqual($0 as? InstallError, .gameRunning)
        }
        XCTAssertFalse(mac.fm.fileExists(atPath: game.appendingPathComponent("d3d12.dll").path))
    }

    func testRefusesAFolderWithoutTheGame() throws {
        let mac = try FakeMac()
        let installer = try mac.installer()
        XCTAssertThrowsError(try installer.install(GameInstall(folder: mac.home, bottle: nil, candidates: []))) {
            XCTAssertEqual($0 as? InstallError, .notAGameFolder)
        }
    }

    func testReadOnlyFolder() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        try mac.fm.setAttributes([.posixPermissions: 0o555], ofItemAtPath: game.path)
        defer { try? mac.fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: game.path) }
        guard getuid() != 0 else { throw XCTSkip("root can write anywhere") }
        let installer = try mac.installer()
        XCTAssertThrowsError(try installer.install(GameInstall(folder: game, bottle: nil, candidates: []))) {
            XCTAssertEqual($0 as? InstallError, .readOnly(game.path))
        }
    }
}

final class BottleSignalTests: XCTestCase {
    /// Two bottles with Steam, game on an external drive that no Steam library lists: the
    /// bottle where the game was set up before wins.
    func testPrefersTheBottleWhereTheGameWasSetUp() throws {
        let mac = try FakeMac()
        try mac.bottle("Other")
        try mac.bottle("Steam", userReg: "WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\AppDefaults\\\\Spider-Man2.exe\\\\DllOverrides] 1\n\"d3d12\"=\"native,builtin\"\n")
        let game = try mac.game(in: mac.volumes.appendingPathComponent("MPT/SteamLibrary"))
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(install.bottle?.name, "Steam")
        XCTAssertEqual(install.reason, .gameSettings)
        XCTAssertEqual(install.candidates.map(\.name), ["Other", "Steam"])
    }

    /// The bottle with a drive letter for the game's disk (Z: = / doesn't count).
    func testPrefersTheBottleWithADriveLetterForTheDisk() throws {
        let mac = try FakeMac()
        let other = try mac.bottle("Other")
        let steam = try mac.bottle("Steam")
        let drive = mac.volumes.appendingPathComponent("MPT")
        let game = try mac.game(in: drive.appendingPathComponent("SteamLibrary"))
        try mac.drive("z", in: other, to: URL(fileURLWithPath: "/"))
        try mac.drive("z", in: steam, to: URL(fileURLWithPath: "/"))
        try mac.drive("k", in: steam, to: drive)
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(install.bottle?.name, "Steam")
        XCTAssertEqual(install.reason, .driveLetter)
    }

    /// Newer Steam keeps the library list in config/libraryfolders.vdf too.
    func testReadsTheConfigLibraryList() throws {
        let mac = try FakeMac()
        try mac.bottle("Other")
        let steam = try mac.bottle("Steam")
        let drive = mac.volumes.appendingPathComponent("MPT")
        let game = try mac.game(in: drive.appendingPathComponent("SteamLibrary"))
        try mac.drive("k", in: steam, to: drive)
        let config = steam.appendingPathComponent("drive_c/Program Files (x86)/Steam/config")
        try mac.fm.createDirectory(at: config, withIntermediateDirectories: true)
        try "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"K:\\\\SteamLibrary\"\n\t}\n}\n"
            .write(to: config.appendingPathComponent("libraryfolders.vdf"), atomically: true, encoding: .utf8)
        let install = GameFinder.install(at: game, env: mac.env)
        XCTAssertEqual(install.bottle?.name, "Steam")
        XCTAssertEqual(install.reason, .steamLibrary)
    }
}

final class ShaderBuilderTests: XCTestCase {
    /// Both stand-ins come out with raw loads only, validated and signed by dxv.
    func testRewritesTypedPoolReadsToRaw() throws {
        guard let tools = StandIns.tools, let standIns = try StandIns.shared() else { throw XCTSkip("needs SM2FIX_DXC") }
        let builder = ShaderBuilder(tools: tools)
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("sm2fix-rewrite-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: work) }
        for (container, shader) in zip(standIns.containers, standIns.shaders) {
            let rewritten = try builder.rewrite(container, space: shader.space, register: shader.register)
            XCTAssertEqual(shaderHash(rewritten), shader.resultHash, "deterministic")
            let file = work.appendingPathComponent(shader.fileName), listing = work.appendingPathComponent("out.ll")
            try rewritten.write(to: file)
            XCTAssertEqual(ProcessRunner().run(tools.appendingPathComponent("dxc"), ["-dumpbin", file.path, "-Fc", listing.path],
                                               environment: [:], timeout: 60).status, 0)
            let text = try String(contentsOf: listing, encoding: .utf8)
            XCTAssertTrue(text.contains("@dx.op.rawBufferLoad.i32(i32 139"))
            XCTAssertFalse(text.contains("@dx.op.bufferLoad.i32("))
            XCTAssertTrue(text.contains("ResourceProperties { i32 11, i32 0 }"))
        }
    }

    func testFindsTheShaderInsideTheExe() throws {
        guard let tools = StandIns.tools, let standIns = try StandIns.shared() else { throw XCTSkip("needs SM2FIX_DXC") }
        let exe = Data(repeating: 7, count: 100) + standIns.containers[1] + Data("DXBC junk".utf8) + standIns.containers[0]
        let builder = ShaderBuilder(tools: tools)
        XCTAssertEqual(builder.extract(standIns.shaders[0], from: exe), standIns.containers[0])
        XCTAssertEqual(builder.extract(standIns.shaders[1], from: exe), standIns.containers[1])
        XCTAssertNil(builder.extract(PoolShader.spiderMan2[0], from: exe))
    }

    /// A game version without the shaders: nothing in the game folder changes.
    func testUnsupportedGameVersionChangesNothing() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"), withShaders: false)
        let installer = try mac.installer()
        XCTAssertThrowsError(try installer.install(GameInstall(folder: game, bottle: nil, candidates: []))) {
            XCTAssertEqual($0 as? InstallError, .shaders(.shaderNotFound))
        }
        XCTAssertEqual(try mac.fm.contentsOfDirectory(atPath: game.path).sorted(), ["Spider-Man2.exe", "dstorage.dll"])
    }

    /// A rebuilt shader that doesn't match the verified one is refused.
    func testRefusesAnUnverifiedResult() throws {
        let mac = try FakeMac()
        let game = try mac.game(in: mac.volumes.appendingPathComponent("Ext"))
        let base = try mac.installer()
        var shaders = base.shaders
        shaders[0].resultHash = 1
        let installer = Installer(payload: base.payload, env: mac.env, shaders: shaders)
        XCTAssertThrowsError(try installer.install(GameInstall(folder: game, bottle: nil, candidates: []))) {
            XCTAssertEqual($0 as? InstallError, .shaders(.unexpectedResult))
        }
        XCTAssertFalse(mac.fm.fileExists(atPath: game.appendingPathComponent("d3d12.dll").path))
    }

    /// The pipeline (dxc -dumpbin, rewrite, dxa, dxv) must give the same bytes on the Mac
    /// as with the Linux build of the same DXC commit that produced the verified game
    /// shaders. Fixtures/standin-t1.dxil was compiled from standin-t1.hlsl with that build;
    /// f48be14de76e162e is its rewritten result there.
    func testRewriteMatchesTheReferenceBuild() throws {
        guard let tools = StandIns.tools else { throw XCTSkip("needs SM2FIX_DXC") }
        let fixture = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("Fixtures/standin-t1.dxil")
        let container = try Data(contentsOf: fixture)
        XCTAssertEqual(hex(shaderHash(container)), "0f7cb4aa736453cf")
        let rewritten = try ShaderBuilder(tools: tools).rewrite(container, space: 0, register: 1)
        XCTAssertEqual(hex(shaderHash(rewritten)), "f48be14de76e162e")
    }

    func testRealShaderList() {
        XCTAssertEqual(PoolShader.spiderMan2.map(\.fileName),
                       ["sm2fix-pool-3d50acdb571dce22.dxil", "sm2fix-pool-14902794dd7134e4.dxil"])
    }
}
