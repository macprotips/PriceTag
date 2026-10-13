import CardStockKit
import Foundation

enum WidgetData {
    /// Refresh in the widget only if prices are older than this, which means
    /// the app hasn't run for a while.
    static let maxAge: TimeInterval = 6 * 60 * 60
    /// Ask WidgetKit to reload this often. Sources update daily, so this is plenty.
    static let reloadInterval: TimeInterval = 3 * 60 * 60
    /// Widgets get little time to run. Past this, show the saved prices.
    static let refreshBudget: Duration = .seconds(20)

    /// The latest snapshot, refreshing prices first if they're old. Every
    /// widget on the desktop shares one refresh.
    static func snapshot() async -> WidgetSnapshot {
        await Coordinator.shared.snapshot()
    }

    static func empty(_ currency: Currency) -> WidgetSnapshot {
        WidgetSnapshot(generated: Date(), currency: currency, lastRefresh: nil, rows: [])
    }

    static func thumbnailURL(_ row: WidgetSnapshot.Row) -> URL? {
        row.thumbnail.map { SharedContainer.thumbnailsURL.appendingPathComponent($0) }
    }

    /// Makes sure several widgets reloading at once share one refresh.
    private actor Coordinator {
        static let shared = Coordinator()
        private var inFlight: Task<WidgetSnapshot, Never>?

        func snapshot() async -> WidgetSnapshot {
            if let inFlight { return await inFlight.value }
            let task = Task { await WidgetData.load() }
            inFlight = task
            let snapshot = await task.value
            inFlight = nil
            return snapshot
        }
    }

    private static func load() async -> WidgetSnapshot {
        let currency = Preferences.currency
        let saved = WidgetSnapshot.load()
        if let saved, saved.currency == currency,
           let last = saved.lastRefresh, Date().timeIntervalSince(last) < maxAge {
            return saved
        }

        // Refresh, but give up after the budget and show what's saved.
        let refreshed = await withTaskGroup(of: WidgetSnapshot?.self) { group in
            group.addTask {
                let store = WatchlistStore(fileURL: SharedContainer.watchlistURL)
                guard let document = try? await PriceRefresher().refresh(store: store, olderThan: maxAge) else { return nil }
                guard !Task.isCancelled else { return nil }
                let snapshot = WidgetSnapshot.make(from: document, currency: currency)
                try? snapshot.write()
                return snapshot
            }
            group.addTask {
                try? await Task.sleep(for: refreshBudget)
                return nil
            }
            let first = await group.next() ?? nil
            group.cancelAll()
            return first
        }
        return refreshed ?? saved ?? empty(currency)
    }
}
