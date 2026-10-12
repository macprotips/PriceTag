import CardStockKit
import Foundation

enum WidgetData {
    /// Refresh in the widget only if the app hasn't for this long.
    static let maxAge: TimeInterval = 6 * 60 * 60
    /// Ask WidgetKit to reload this often. Sources update daily, so this is plenty.
    static let reloadInterval: TimeInterval = 3 * 60 * 60

    /// The latest snapshot. If prices are old (the app hasn't run for a
    /// while), refreshes them first and saves the result for the app too.
    static func snapshot() async -> WidgetSnapshot {
        let currency = Preferences.currency
        if let saved = WidgetSnapshot.load(), saved.currency == currency,
           let last = saved.lastRefresh, Date().timeIntervalSince(last) < maxAge {
            return saved
        }

        let store = WatchlistStore(fileURL: SharedContainer.watchlistURL)
        guard let document = try? await PriceRefresher().refresh(store: store, olderThan: maxAge) else {
            return WidgetSnapshot.load() ?? empty(currency)
        }
        await ThumbnailCache().fetchMissing(for: document.items.map(\.card))
        let snapshot = WidgetSnapshot.make(from: document, currency: currency)
        try? snapshot.write()
        return snapshot
    }

    static func empty(_ currency: Currency) -> WidgetSnapshot {
        WidgetSnapshot(generated: Date(), currency: currency, lastRefresh: nil, rows: [])
    }

    static func thumbnailURL(_ row: WidgetSnapshot.Row) -> URL? {
        row.thumbnail.map { SharedContainer.thumbnailsURL.appendingPathComponent($0) }
    }
}
