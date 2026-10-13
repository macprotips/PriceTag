import Foundation

/// Everything CardStock saves: the watchlist and its price history.
public struct WatchlistDocument: Codable, Equatable, Sendable {
    public static let currentVersion = 1

    public var version: Int
    public var items: [WatchItem]
    /// When prices were last refreshed, by the app or the widget.
    public var lastRefresh: Date?

    public init(items: [WatchItem] = [], lastRefresh: Date? = nil) {
        version = Self.currentVersion
        self.items = items
        self.lastRefresh = lastRefresh
    }

    enum CodingKeys: String, CodingKey {
        case version, items, lastRefresh
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        version = try container.decodeIfPresent(Int.self, forKey: .version) ?? 1
        guard version <= Self.currentVersion else {
            throw WatchlistStoreError.newerVersion(version)
        }
        // One unreadable card shouldn't cost the rest of the watchlist.
        items = try container.decode([Lenient<WatchItem>].self, forKey: .items).compactMap(\.value)
        lastRefresh = try container.decodeIfPresent(Date.self, forKey: .lastRefresh)
    }

    /// Takes refreshed copies of items that are still on the list. Items added
    /// or removed while the refresh was running are left as they are.
    public mutating func merge(refreshed: [WatchItem]) {
        let byID = Dictionary(refreshed.map { ($0.id, $0) }, uniquingKeysWith: { _, latest in latest })
        items = items.map { byID[$0.id] ?? $0 }
    }
}

public enum WatchlistStoreError: Error, LocalizedError, Equatable {
    /// The file was saved by a newer version of CardStock. It's left alone.
    case newerVersion(Int)

    public var errorDescription: String? {
        switch self {
        case .newerVersion: "Your watchlist was saved by a newer version of CardStock. Update CardStock to open it."
        }
    }
}

/// Decodes a value, or nil if it can't be decoded, instead of failing the
/// whole array it's in.
struct Lenient<Value: Decodable>: Decodable {
    let value: Value?

    init(from decoder: Decoder) throws {
        value = try? Value(from: decoder)
    }
}

/// Reads and writes the watchlist file.
///
/// The app and the widget both use the file, so access goes through
/// `NSFileCoordinator` and every write is atomic. Being an actor keeps writes
/// from one process in order.
public actor WatchlistStore {
    public let fileURL: URL

    public init(fileURL: URL) {
        self.fileURL = fileURL
    }

    /// The saved watchlist, or an empty one if nothing is saved yet.
    public func load() throws -> WatchlistDocument {
        var result: Result<WatchlistDocument, Error> = .success(WatchlistDocument())
        var coordinationError: NSError?
        NSFileCoordinator().coordinate(readingItemAt: fileURL, options: [], error: &coordinationError) { url in
            result = Result { try Self.read(url) }
        }
        if let coordinationError { throw coordinationError }
        return try result.get()
    }

    public func save(_ document: WatchlistDocument) throws {
        var result: Result<Void, Error> = .success(())
        var coordinationError: NSError?
        NSFileCoordinator().coordinate(writingItemAt: fileURL, options: .forReplacing, error: &coordinationError) { url in
            result = Result { try Self.write(document, to: url) }
        }
        if let coordinationError { throw coordinationError }
        try result.get()
    }

    /// Reads, changes and writes the file as one step, so a change made by the
    /// other process in between isn't lost.
    @discardableResult
    public func update(_ change: @Sendable (inout WatchlistDocument) -> Void) throws -> WatchlistDocument {
        var result: Result<WatchlistDocument, Error> = .success(WatchlistDocument())
        var coordinationError: NSError?
        NSFileCoordinator().coordinate(writingItemAt: fileURL, options: .forMerging, error: &coordinationError) { url in
            result = Result {
                var document = try Self.read(url)
                change(&document)
                try Self.write(document, to: url)
                return document
            }
        }
        if let coordinationError { throw coordinationError }
        return try result.get()
    }

    /// Moves a file that can't be read out of the way, so nothing overwrites
    /// it, and returns where it went.
    public func setAsideDamagedFile(now: Date = Date()) throws -> URL {
        let stamp = ISO8601DateFormatter().string(from: now).replacingOccurrences(of: ":", with: "-")
        let backup = fileURL.deletingLastPathComponent()
            .appendingPathComponent("watchlist (unreadable \(stamp)).json")
        try FileManager.default.moveItem(at: fileURL, to: backup)
        return backup
    }

    private static func read(_ url: URL) throws -> WatchlistDocument {
        guard FileManager.default.fileExists(atPath: url.path) else { return WatchlistDocument() }
        let data = try Data(contentsOf: url)
        return try JSONDecoder().decode(WatchlistDocument.self, from: data)
    }

    private static func write(_ document: WatchlistDocument, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        try encoder.encode(document).write(to: url, options: .atomic)
    }
}
