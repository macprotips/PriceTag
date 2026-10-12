import Foundation

/// Fetches fresh prices for watchlist items from the right source for each game.
public struct PriceRefresher: Sendable {
    public struct Result: Sendable {
        /// Every item passed in, updated where the refresh succeeded.
        public var items: [WatchItem]
        /// Sources that couldn't be reached at all, with the reason.
        public var failures: [DataSource: String]
    }

    private let providers: [Game: any PriceProvider]

    public init(providers: [any PriceProvider] = [ScryfallProvider(), TCGdexProvider()]) {
        self.providers = Dictionary(providers.map { ($0.game, $0) }, uniquingKeysWith: { first, _ in first })
    }

    public func provider(for game: Game) -> (any PriceProvider)? {
        providers[game]
    }

    public func refresh(_ items: [WatchItem], now: Date = Date(), calendar: Calendar = .current) async -> Result {
        let games = Set(items.map(\.card.game))
        let batches = await withTaskGroup(of: (Game, Swift.Result<QuoteBatch, Error>).self,
                                          returning: [Game: Swift.Result<QuoteBatch, Error>].self) { group in
            for game in games {
                guard let provider = providers[game] else { continue }
                let ids = items.filter { $0.card.game == game }.map(\.card.sourceID)
                group.addTask {
                    do { return (game, .success(try await provider.quotes(for: ids))) }
                    catch { return (game, .failure(error)) }
                }
            }
            var batches: [Game: Swift.Result<QuoteBatch, Error>] = [:]
            for await (game, result) in group {
                batches[game] = result
            }
            return batches
        }

        var failures: [DataSource: String] = [:]
        let updated = items.map { item -> WatchItem in
            var item = item
            switch batches[item.card.game] {
            case .some(.success(let batch)):
                if let quote = batch.quotes[item.card.sourceID] {
                    item.apply(quote, at: now, calendar: calendar)
                } else {
                    item.lastError = batch.failures[item.card.sourceID] ?? "No price right now."
                }
            case .some(.failure(let error)):
                let message = error.localizedDescription
                item.lastError = message
                failures[item.card.game.source] = message
            case .none:
                item.lastError = "No price source for \(item.card.game.name)."
            }
            return item
        }
        return Result(items: updated, failures: failures)
    }

    /// Refreshes the saved watchlist if it's older than `maxAge` (or always,
    /// if `maxAge` is nil) and saves the result. Used by the widget when the
    /// app hasn't run for a while.
    @discardableResult
    public func refresh(store: WatchlistStore, olderThan maxAge: TimeInterval?, now: Date = Date()) async throws -> WatchlistDocument {
        let document = try await store.load()
        if let maxAge, let last = document.lastRefresh, now.timeIntervalSince(last) < maxAge {
            return document
        }
        guard !document.items.isEmpty else { return document }
        let result = await refresh(document.items, now: now)
        let refreshed = result.items
        return try await store.update { document in
            document.merge(refreshed: refreshed)
            document.lastRefresh = now
        }
    }
}
