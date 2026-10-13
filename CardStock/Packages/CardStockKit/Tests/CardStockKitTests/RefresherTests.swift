@testable import CardStockKit
import XCTest

/// A provider that answers from memory.
private struct StubProvider: PriceProvider {
    let game: Game
    var quotes: [String: CardQuote] = [:]
    var error: ProviderError?

    func search(_ query: String) async throws -> [CardQuote] { Array(quotes.values) }

    func quote(for sourceID: String) async throws -> CardQuote {
        if let error { throw error }
        guard let quote = quotes[sourceID] else { throw ProviderError.notFound }
        return quote
    }

    func quotes(for sourceIDs: [String]) async throws -> QuoteBatch {
        if let error { throw error }
        var batch = QuoteBatch()
        for id in sourceIDs {
            if let quote = quotes[id] { batch.quotes[id] = quote } else { batch.failures[id] = "Gone" }
        }
        return batch
    }
}

final class RefresherTests: XCTestCase {
    func testRefreshesEachGameFromItsSource() async {
        let magic = sampleCard("m", game: .magic)
        let pokemon = sampleCard("p", game: .pokemon)
        let missing = sampleCard("x", game: .magic)
        let refresher = PriceRefresher(providers: [
            StubProvider(game: .magic, quotes: ["m": CardQuote(card: magic, prices: [.nonfoil: Price(usd: Amount(cents: 150))])]),
            StubProvider(game: .pokemon, error: .offline),
        ])
        let items = [magic, pokemon, missing].map { WatchItem(card: $0, finish: .nonfoil) }

        let result = await refresher.refresh(items, now: day(3), calendar: utc)

        XCTAssertEqual(result.items[0].price?.usd, Amount(cents: 150))
        XCTAssertEqual(result.items[0].lastUpdated, day(3))
        XCTAssertNil(result.items[0].lastError)
        XCTAssertEqual(result.items[1].lastError, ProviderError.offline.localizedDescription)
        XCTAssertEqual(result.items[2].lastError, "Gone")
        XCTAssertEqual(result.failures.keys.sorted(by: { $0.rawValue < $1.rawValue }), [.tcgdex])
        XCTAssertEqual(result.updatedCount, 1)
    }

    func testStoreRefreshSkipsFreshData() async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".json")
        defer { try? FileManager.default.removeItem(at: url) }
        let card = sampleCard("m")
        let store = WatchlistStore(fileURL: url)
        try await store.save(WatchlistDocument(items: [WatchItem(card: card, finish: .nonfoil)], lastRefresh: day(3, hour: 10)))
        let refresher = PriceRefresher(providers: [
            StubProvider(game: .magic, quotes: ["m": CardQuote(card: card, prices: [.nonfoil: Price(usd: Amount(cents: 150))])]),
        ])

        let fresh = try await refresher.refresh(store: store, olderThan: 6 * 3600, now: day(3, hour: 12))
        XCTAssertNil(fresh.items[0].price, "Two hours old, so not refreshed")

        let stale = try await refresher.refresh(store: store, olderThan: 6 * 3600, now: day(3, hour: 20))
        XCTAssertEqual(stale.items[0].price?.usd, Amount(cents: 150))
        XCTAssertEqual(stale.lastRefresh, day(3, hour: 20))
    }
}

final class RefreshHonestyTests: XCTestCase {
    func testFailedRefreshDoesNotLookFresh() async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".json")
        defer { try? FileManager.default.removeItem(at: url) }
        let store = WatchlistStore(fileURL: url)
        try await store.save(WatchlistDocument(items: [WatchItem(card: sampleCard("m"), finish: .nonfoil)], lastRefresh: day(1)))
        let offline = PriceRefresher(providers: [OfflineProvider()])

        let document = try await offline.refresh(store: store, olderThan: nil, now: day(5))
        XCTAssertEqual(document.lastRefresh, day(1))
        XCTAssertNotNil(document.items[0].lastError)
    }
}

private struct OfflineProvider: PriceProvider {
    let game = Game.magic
    func search(_ query: String) async throws -> [CardQuote] { throw ProviderError.offline }
    func quote(for sourceID: String) async throws -> CardQuote { throw ProviderError.offline }
    func quotes(for sourceIDs: [String]) async throws -> QuoteBatch { throw ProviderError.offline }
}
