import CryptoKit
import Foundation

/// One exact byte patch for one exact version of one file.
public struct PatchSpec: Sendable {
    public let fileName: String
    public let offset: Int
    public let originalBytes: [UInt8]
    public let replacementBytes: [UInt8]
    public let originalSHA256: String
    public let patchedSHA256: String

    public init(fileName: String, offset: Int, originalBytes: [UInt8], replacementBytes: [UInt8],
                originalSHA256: String, patchedSHA256: String) {
        self.fileName = fileName
        self.offset = offset
        self.originalBytes = originalBytes
        self.replacementBytes = replacementBytes
        self.originalSHA256 = originalSHA256.lowercased()
        self.patchedSHA256 = patchedSHA256.lowercased()
    }

    /// Turns off the optional graphics cache behind the startup black screen
    /// in Need for Speed Unbound, Steam build 16690907, under CrossOver with D3DMetal.
    public static let unboundBlackScreen = PatchSpec(
        fileName: "Engine.Render.Core2.PlatformPcDx12.dll",
        offset: 0x1AB423,
        originalBytes: [0x44, 0x38, 0xAB, 0xC0, 0x01, 0x00, 0x00],
        replacementBytes: [0x80, 0xA3, 0xC0, 0x01, 0x00, 0x00, 0x00],
        originalSHA256: "ee3a654be58be406eda32aeadad99ad60894403e374a2d3b847a20008cd30c62",
        patchedSHA256: "9dc33b2edebca402c48c53f32cce474aafd5d640e5754275484deaced66134aa"
    )
}

public enum FileState: Equatable, Sendable {
    /// The exact file the fix was made for. Safe to patch.
    case original
    /// The fix is already applied.
    case patched
    /// Some other version. Never touched.
    case unsupported(sha256: String)
}

public enum PatchError: LocalizedError, Equatable {
    case notFound(fileName: String)
    case multipleFound([URL])
    case unsupportedVersion(sha256: String)
    case alreadyPatched
    case notPatched
    case unexpectedBytes
    case patchHashMismatch
    case notWritable(URL)
    case backupFailed
    case noBackup
    case verifyFailedRestored
    case verifyFailedNotRestored(backup: URL)

    public var errorDescription: String? {
        switch self {
        case .notFound(let name):
            return "Couldn't find \(name) in what you dropped. Drop the Need for Speed Unbound game folder, or your CrossOver bottle."
        case .multipleFound(let urls):
            let list = urls.map { "• " + $0.path }.joined(separator: "\n")
            return "Found more than one copy of the game file. Drop the one game folder you want to fix.\n\n\(list)"
        case .unsupportedVersion:
            return "This game file isn't the version the fix was made for (Steam build 16690907), so nothing was changed. A game update may have replaced it."
        case .alreadyPatched:
            return "The fix is already applied."
        case .notPatched:
            return "The fix isn't applied, so there's nothing to undo."
        case .unexpectedBytes:
            return "The bytes at the patch location aren't what was expected, so nothing was changed."
        case .patchHashMismatch:
            return "The patched file didn't come out as expected, so nothing was changed."
        case .notWritable(let url):
            return "macOS won't let this app change \(url.lastPathComponent). Nothing was changed."
        case .backupFailed:
            return "Couldn't make a verified backup, so nothing was changed."
        case .noBackup:
            return "No verified backup of the original file was found. You can get the original back with Steam's \"Verify integrity of game files\"."
        case .verifyFailedRestored:
            return "The file didn't check out after saving, so the original was put back. Nothing has changed."
        case .verifyFailedNotRestored(let backup):
            return "The file didn't check out after saving and the original couldn't be put back automatically. Your backup is at \(backup.path). Copy it back into the game folder, or use Steam's \"Verify integrity of game files\"."
        }
    }
}

public func sha256Hex(of data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

public func sha256Hex(ofFileAt url: URL) throws -> String {
    let handle = try FileHandle(forReadingFrom: url)
    defer { try? handle.close() }
    var hasher = SHA256()
    while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
        hasher.update(data: chunk)
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}

public struct Patcher: Sendable {
    public let spec: PatchSpec
    /// Where verified copies of the original file are kept. Outside the game folder,
    /// so Steam updates and verification don't touch them.
    public let backupDirectory: URL

    public init(spec: PatchSpec, backupDirectory: URL) {
        self.spec = spec
        self.backupDirectory = backupDirectory
    }

    public static var defaultBackupDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Unbound Fix/Backups", isDirectory: true)
    }

    // MARK: Finding the file

    /// Every copy of the target file in `url`, which can be the file itself, the game
    /// folder, or anything above it such as a CrossOver bottle.
    public func findTargets(in url: URL) -> [URL] {
        let root = url.resolvingSymlinksInPath()
        let keys: [URLResourceKey] = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
        let rootValues = try? root.resourceValues(forKeys: Set(keys))
        if rootValues?.isDirectory != true {
            return isTargetName(root) ? [root] : []
        }

        let backupPath = backupDirectory.resolvingSymlinksInPath().path
        guard let enumerator = FileManager.default.enumerator(
            at: root, includingPropertiesForKeys: keys, options: [], errorHandler: { _, _ in true }
        ) else { return [] }

        var found: [URL] = []
        for case let item as URL in enumerator {
            let values = try? item.resourceValues(forKeys: Set(keys))
            // Bottles link back to the whole Mac (dosdevices, Desktop, Documents…). Never follow links.
            if values?.isSymbolicLink == true || item.lastPathComponent == "dosdevices" {
                enumerator.skipDescendants()
                continue
            }
            if values?.isDirectory == true, item.path.hasPrefix(backupPath) {
                enumerator.skipDescendants()
                continue
            }
            if values?.isRegularFile == true, isTargetName(item) {
                found.append(item)
            }
        }
        return found
    }

    /// The single copy of the target file in `url`.
    public func findTarget(in url: URL) throws -> URL {
        let found = findTargets(in: url)
        switch found.count {
        case 0: throw PatchError.notFound(fileName: spec.fileName)
        case 1: return found[0]
        default: throw PatchError.multipleFound(found)
        }
    }

    private func isTargetName(_ url: URL) -> Bool {
        url.lastPathComponent.caseInsensitiveCompare(spec.fileName) == .orderedSame
    }

    // MARK: Checking

    public func state(of file: URL) throws -> FileState {
        state(forHash: try sha256Hex(ofFileAt: file))
    }

    private func state(forHash hash: String) -> FileState {
        switch hash {
        case spec.originalSHA256: return .original
        case spec.patchedSHA256: return .patched
        default: return .unsupported(sha256: hash)
        }
    }

    /// A backup whose contents match the original file exactly.
    public func existingBackup() -> URL? {
        guard let enumerator = FileManager.default.enumerator(
            at: backupDirectory, includingPropertiesForKeys: [.isRegularFileKey]
        ) else { return nil }
        for case let item as URL in enumerator where isTargetName(item) {
            if (try? sha256Hex(ofFileAt: item)) == spec.originalSHA256 {
                return item
            }
        }
        return nil
    }

    // MARK: Applying and undoing

    /// Backs up the original, applies the patch and checks the result.
    /// Returns the backup. On any failure the game file is left as it was.
    @discardableResult
    public func apply(to file: URL) throws -> URL {
        let original = try Data(contentsOf: file)
        switch state(forHash: sha256Hex(of: original)) {
        case .original: break
        case .patched: throw PatchError.alreadyPatched
        case .unsupported(let hash): throw PatchError.unsupportedVersion(sha256: hash)
        }

        let range = spec.offset ..< spec.offset + spec.originalBytes.count
        guard spec.originalBytes.count == spec.replacementBytes.count,
              original.count >= range.upperBound,
              Array(original[range]) == spec.originalBytes
        else { throw PatchError.unexpectedBytes }

        var patched = original
        patched.replaceSubrange(range, with: spec.replacementBytes)
        guard sha256Hex(of: patched) == spec.patchedSHA256 else { throw PatchError.patchHashMismatch }

        try checkWritable(file)
        let backup = try existingBackup() ?? makeBackup(of: original)

        try replaceContents(of: file, with: patched)
        if (try? sha256Hex(ofFileAt: file)) != spec.patchedSHA256 {
            do {
                try replaceContents(of: file, with: original)
            } catch {
                throw PatchError.verifyFailedNotRestored(backup: backup)
            }
            throw PatchError.verifyFailedRestored
        }
        return backup
    }

    /// Puts the verified backup of the original back. Only works on a patched file,
    /// so it never overwrites a newer version from a game update.
    public func restore(_ file: URL) throws {
        switch try state(of: file) {
        case .patched: break
        case .original: throw PatchError.notPatched
        case .unsupported(let hash): throw PatchError.unsupportedVersion(sha256: hash)
        }
        guard let backup = existingBackup() else { throw PatchError.noBackup }
        let original = try Data(contentsOf: backup)
        guard sha256Hex(of: original) == spec.originalSHA256 else { throw PatchError.noBackup }

        try checkWritable(file)
        try replaceContents(of: file, with: original)
        guard try state(of: file) == .original else {
            throw PatchError.verifyFailedNotRestored(backup: backup)
        }
    }

    private func makeBackup(of original: Data) throws -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let folder = backupDirectory.appendingPathComponent(formatter.string(from: Date()), isDirectory: true)
        let backup = folder.appendingPathComponent(spec.fileName)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try original.write(to: backup, options: .withoutOverwriting)
        } catch {
            throw PatchError.backupFailed
        }
        guard (try? sha256Hex(ofFileAt: backup)) == spec.originalSHA256 else { throw PatchError.backupFailed }
        return backup
    }

    private func checkWritable(_ file: URL) throws {
        let fm = FileManager.default
        guard fm.isWritableFile(atPath: file.path),
              fm.isWritableFile(atPath: file.deletingLastPathComponent().path)
        else { throw PatchError.notWritable(file) }
    }

    /// Writes to a temporary file next to `file`, then swaps it in, so the game file
    /// is never left half written.
    private func replaceContents(of file: URL, with data: Data) throws {
        let temp = file.deletingLastPathComponent()
            .appendingPathComponent(".\(file.lastPathComponent).\(UUID().uuidString).tmp")
        do {
            try data.write(to: temp, options: .withoutOverwriting)
            _ = try FileManager.default.replaceItemAt(file, withItemAt: temp)
        } catch {
            try? FileManager.default.removeItem(at: temp)
            throw error
        }
    }
}
