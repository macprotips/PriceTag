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

    /// Takes refreshed copies of items that are still on the list. Items added
    /// or removed while the refresh was running are left as they are.
    public mutating func merge(refreshed: [WatchItem]) {
        let byID = Dictionary(refreshed.map { ($0.id, $0) }, uniquingKeysWith: { _, latest in latest })
        items = items.map { byID[$0.id] ?? $0 }
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
