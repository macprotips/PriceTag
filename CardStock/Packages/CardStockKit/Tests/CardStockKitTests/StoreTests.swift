@testable import CardStockKit
import XCTest

final class StoreTests: XCTestCase {
    private var directory: URL!

    override func setUpWithError() throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: directory)
    }

    func testMissingFileIsAnEmptyWatchlist() async throws {
        let store = WatchlistStore(fileURL: directory.appendingPathComponent("watchlist.json"))
        let document = try await store.load()
        XCTAssertEqual(document, WatchlistDocument())
    }

    func testRoundTrip() async throws {
        let store = WatchlistStore(fileURL: directory.appendingPathComponent("watchlist.json"))
        var item = WatchItem(card: sampleCard(), finish: .foil, added: day(1))
        item.record(Price(usd: Amount(cents: 149), eur: Amount(cents: 120)), at: day(1), calendar: utc)
        let document = WatchlistDocument(items: [item], lastRefresh: day(1))

        try await store.save(document)
        let loaded = try await store.load()
        XCTAssertEqual(loaded, document)
    }

    func testUpdateReadsLatestFile() async throws {
        let url = directory.appendingPathComponent("watchlist.json")
        let app = WatchlistStore(fileURL: url)
        let widget = WatchlistStore(fileURL: url)

        try await app.save(WatchlistDocument(items: [WatchItem(card: sampleCard("a"), finish: .nonfoil)]))
        try await widget.update { $0.lastRefresh = day(5) }
        let document = try await app.load()

        XCTAssertEqual(document.items.count, 1)
        XCTAssertEqual(document.lastRefresh, day(5))
    }

    func testMergeKeepsItemsAddedDuringRefresh() {
        let a = WatchItem(card: sampleCard("a"), finish: .nonfoil)
        let b = WatchItem(card: sampleCard("b"), finish: .nonfoil)
        var refreshedA = a
        refreshedA.price = Price(usd: Amount(cents: 999))

        var document = WatchlistDocument(items: [a, b])
        document.merge(refreshed: [refreshedA])

        XCTAssertEqual(document.items.map(\.id), [a.id, b.id])
        XCTAssertEqual(document.items[0].price?.usd, Amount(cents: 999))
        XCTAssertNil(document.items[1].price)
    }

    func testSnapshot() throws {
        var item = WatchItem(card: sampleCard(), finish: .foil, added: day(1))
        item.record(Price(usd: Amount(cents: 100)), at: day(1), calendar: utc)
        item.record(Price(usd: Amount(cents: 110)), at: day(2), calendar: utc)
        let document = WatchlistDocument(items: [item], lastRefresh: day(2))

        let snapshot = WidgetSnapshot.make(from: document, currency: .usd, now: day(2), calendar: utc)
        let row = try XCTUnwrap(snapshot.rows.first)
        XCTAssertEqual(row.name, "Sol Ring")
        XCTAssertEqual(row.subtitle, "CMM · Foil")
        XCTAssertEqual(row.price, Amount(cents: 110))
        XCTAssertEqual(row.change?.amount, Amount(cents: 10))
        XCTAssertEqual(row.sparkline, [1.0, 1.1])
        XCTAssertEqual(row.thumbnail, "magic_abc.img")

        let url = directory.appendingPathComponent("snapshot.json")
        try snapshot.write(to: url)
        XCTAssertEqual(WidgetSnapshot.load(from: url), snapshot)
    }
}
