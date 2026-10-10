import Foundation
import UnboundFixKit
import XCTest

final class PatcherTests: XCTestCase {
    private var root: URL!
    private var gameFolder: URL!
    private var backups: URL!

    private let offset = 1000
    private let originalBytes: [UInt8] = [0x44, 0x38, 0xAB, 0xC0, 0x01, 0x00, 0x00]
    private let replacementBytes: [UInt8] = [0x80, 0xA3, 0xC0, 0x01, 0x00, 0x00, 0x00]
    private var originalData: Data!
    private var patchedData: Data!
    private var spec: PatchSpec!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        gameFolder = root.appendingPathComponent("Bottle/drive_c/Steam/steamapps/common/Need for Speed Unbound")
        backups = root.appendingPathComponent("Backups")
        try FileManager.default.createDirectory(at: gameFolder, withIntermediateDirectories: true)

        var bytes = (0 ..< 4096).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ 7) }
        bytes.replaceSubrange(offset ..< offset + 7, with: originalBytes)
        originalData = Data(bytes)
        patchedData = originalData
        patchedData.replaceSubrange(offset ..< offset + 7, with: replacementBytes)
        spec = makeSpec(originalBytes: originalBytes)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func makeSpec(originalBytes: [UInt8]) -> PatchSpec {
        PatchSpec(fileName: PatchSpec.unboundBlackScreen.fileName, offset: offset,
                  originalBytes: originalBytes, replacementBytes: replacementBytes,
                  originalSHA256: sha256Hex(of: originalData), patchedSHA256: sha256Hex(of: patchedData))
    }

    private var patcher: Patcher { Patcher(spec: spec, backupDirectory: backups) }

    @discardableResult
    private func writeDLL(_ data: Data, in folder: URL? = nil) throws -> URL {
        let url = (folder ?? gameFolder).appendingPathComponent(spec.fileName)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url)
        return url
    }

    func testRealSpecIsConsistent() {
        let real = PatchSpec.unboundBlackScreen
        XCTAssertEqual(real.offset, 0x1AB423)
        XCTAssertEqual(real.originalBytes.count, 7)
        XCTAssertEqual(real.replacementBytes.count, 7)
        XCTAssertEqual(real.originalSHA256.count, 64)
        XCTAssertEqual(real.patchedSHA256.count, 64)
        XCTAssertNotEqual(real.originalSHA256, real.patchedSHA256)
    }

    func testFindsFileFromBottleOrGameFolderOrFileItself() throws {
        let dll = try writeDLL(originalData)
        let bottle = root.appendingPathComponent("Bottle")
        XCTAssertEqual(try patcher.findTarget(in: bottle).resolvingSymlinksInPath(), dll.resolvingSymlinksInPath())
        XCTAssertEqual(try patcher.findTarget(in: gameFolder).resolvingSymlinksInPath(), dll.resolvingSymlinksInPath())
        XCTAssertEqual(try patcher.findTarget(in: dll).resolvingSymlinksInPath(), dll.resolvingSymlinksInPath())
    }

    func testNotFound() {
        XCTAssertThrowsError(try patcher.findTarget(in: gameFolder)) {
            XCTAssertEqual($0 as? PatchError, .notFound(fileName: spec.fileName))
        }
    }

    func testMultipleCopiesIsAnError() throws {
        try writeDLL(originalData)
        try writeDLL(originalData, in: root.appendingPathComponent("Bottle/Other Game"))
        XCTAssertThrowsError(try patcher.findTarget(in: root.appendingPathComponent("Bottle"))) {
            guard case .multipleFound(let urls) = $0 as? PatchError else { return XCTFail("\($0)") }
            XCTAssertEqual(urls.count, 2)
        }
    }

    func testSearchIgnoresBackupsAndSymlinks() throws {
        try writeDLL(originalData)
        try writeDLL(originalData, in: backups.appendingPathComponent("old"))
        let elsewhere = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: elsewhere) }
        try writeDLL(originalData, in: elsewhere)
        try FileManager.default.createSymbolicLink(at: root.appendingPathComponent("link"), withDestinationURL: elsewhere)
        XCTAssertEqual(patcher.findTargets(in: root).count, 1)
    }

    func testApplyThenUndo() throws {
        let dll = try writeDLL(originalData)
        XCTAssertEqual(try patcher.state(of: dll), .original)

        let backup = try patcher.apply(to: dll)
        XCTAssertEqual(try Data(contentsOf: dll), patchedData)
        XCTAssertEqual(try patcher.state(of: dll), .patched)
        XCTAssertEqual(try Data(contentsOf: backup), originalData)
        XCTAssertNotNil(patcher.existingBackup())
        XCTAssertEqual(patcher.findTargets(in: gameFolder).count, 1, "no temp files left behind")

        try patcher.restore(dll)
        XCTAssertEqual(try Data(contentsOf: dll), originalData)
        XCTAssertEqual(try patcher.state(of: dll), .original)
        let leftovers = try FileManager.default.contentsOfDirectory(atPath: gameFolder.path)
        XCTAssertEqual(leftovers, [spec.fileName])
    }

    func testApplyingTwiceIsRefused() throws {
        let dll = try writeDLL(originalData)
        try patcher.apply(to: dll)
        XCTAssertThrowsError(try patcher.apply(to: dll)) { XCTAssertEqual($0 as? PatchError, .alreadyPatched) }
        XCTAssertEqual(try Data(contentsOf: dll), patchedData)
    }

    func testReapplyAfterSteamRestoresOriginalReusesBackup() throws {
        let dll = try writeDLL(originalData)
        let first = try patcher.apply(to: dll)
        try writeDLL(originalData)
        let second = try patcher.apply(to: dll)
        XCTAssertEqual(first.resolvingSymlinksInPath().path, second.resolvingSymlinksInPath().path)
    }

    func testUnsupportedVersionIsNeverTouched() throws {
        var other = originalData!
        other[0] ^= 0xFF
        let dll = try writeDLL(other)
        guard case .unsupported = try patcher.state(of: dll) else { return XCTFail() }
        XCTAssertThrowsError(try patcher.apply(to: dll)) {
            guard case .unsupportedVersion = $0 as? PatchError else { return XCTFail("\($0)") }
        }
        XCTAssertThrowsError(try patcher.restore(dll))
        XCTAssertEqual(try Data(contentsOf: dll), other)
        XCTAssertNil(patcher.existingBackup())
    }

    func testUnexpectedBytesAreRefused() throws {
        spec = makeSpec(originalBytes: [1, 2, 3, 4, 5, 6, 7])
        let dll = try writeDLL(originalData)
        XCTAssertThrowsError(try patcher.apply(to: dll)) { XCTAssertEqual($0 as? PatchError, .unexpectedBytes) }
        XCTAssertEqual(try Data(contentsOf: dll), originalData)
    }

    func testUndoOnUnpatchedFileIsRefused() throws {
        let dll = try writeDLL(originalData)
        XCTAssertThrowsError(try patcher.restore(dll)) { XCTAssertEqual($0 as? PatchError, .notPatched) }
    }

    func testUndoWithoutBackupIsRefused() throws {
        let dll = try writeDLL(patchedData)
        XCTAssertThrowsError(try patcher.restore(dll)) { XCTAssertEqual($0 as? PatchError, .noBackup) }
        XCTAssertEqual(try Data(contentsOf: dll), patchedData)
    }
}
