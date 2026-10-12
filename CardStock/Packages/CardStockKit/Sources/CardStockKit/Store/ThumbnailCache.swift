import Foundation

/// Card images saved to the shared folder, because widgets can't load images
/// from the network while they draw.
public struct ThumbnailCache: Sendable {
    public let directory: URL
    private let session: URLSession

    public init(directory: URL = SharedContainer.thumbnailsURL, session: URLSession = .shared) {
        self.directory = directory
        self.session = session
    }

    /// `"magic:5e1f…"` → `"magic_5e1f….img"`. Card IDs only use letters,
    /// digits, hyphens and dots, but anything else is replaced to be safe.
    public static func fileName(for card: Card) -> String {
        let safe = card.id.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "." ? $0 : "_" }
        return String(safe) + ".img"
    }

    public func fileURL(for card: Card) -> URL {
        directory.appendingPathComponent(Self.fileName(for: card))
    }

    /// Downloads images for cards that don't have one saved yet. Failures are
    /// ignored: the widget just shows no image.
    public func fetchMissing(for cards: [Card]) async {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for card in cards {
            let destination = fileURL(for: card)
            guard let source = card.thumbnailURL,
                  !FileManager.default.fileExists(atPath: destination.path)
            else { continue }
            guard let download = try? await session.data(from: source),
                  (download.1 as? HTTPURLResponse)?.statusCode == 200
            else { continue }
            try? download.0.write(to: destination, options: .atomic)
        }
    }

    /// Deletes images for cards no longer on the watchlist.
    public func removeAll(except cards: [Card]) {
        let keep = Set(cards.map(Self.fileName(for:)))
        let files = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
        for file in files where !keep.contains(file) {
            try? FileManager.default.removeItem(at: directory.appendingPathComponent(file))
        }
    }
}
