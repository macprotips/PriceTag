@testable import CardStockKit
import XCTest

final class CollectionTests: XCTestCase {
    private func owned(_ id: String, quantity: Int, paid: Int? = nil, prices: [(day: Int, cents: Int)]) -> WatchItem {
        var item = WatchItem(card: sampleCard(id), finish: .nonfoil, added: day(prices.first?.day ?? 1))
        for price in prices {
            item.record(Price(usd: Amount(cents: price.cents)), at: day(price.day), calendar: utc)
        }
        item.holding = Holding(quantity: quantity, pricePaid: paid.map(Amount.init(cents:)), currency: .usd)
        return item
    }

    func testNoHoldingsMeansNoSummary() {
        let item = WatchItem(card: sampleCard(), finish: .nonfoil)
        XCTAssertNil(CollectionSummary.make(from: [item], currency: .usd, now: day(10), calendar: utc))
    }

    func testValueGainAndDayChange() throws {
        let a = owned("a", quantity: 2, paid: 400, prices: [(9, 500), (10, 600)])
        let b = owned("b", quantity: 1, prices: [(10, 1000)])
        let watched = WatchItem(card: sampleCard("c"), finish: .nonfoil)

        let summary = try XCTUnwrap(CollectionSummary.make(from: [a, b, watched], currency: .usd, days: 3, now: day(10), calendar: utc))
        XCTAssertEqual(summary.value, Amount(cents: 2200))
        XCTAssertEqual(summary.cardCount, 3)
        XCTAssertEqual(summary.gain?.amount, Amount(cents: 400), "Only cards with a price paid count toward gain")
        XCTAssertEqual(summary.history.map(\.amount.cents), [1000, 2200], "Day 8 had no prices, so it's left out")
        XCTAssertEqual(summary.dayChange?.amount, Amount(cents: 1200))
    }

    func testAlertsFireOnlyWhenCrossing() {
        var before = owned("a", quantity: 1, prices: [(1, 900)])
        before.alert = PriceAlert(above: Amount(cents: 1000), below: Amount(cents: 500), currency: .usd)
        var after = before
        after.record(Price(usd: Amount(cents: 1050)), at: day(2), calendar: utc)

        let hits = PriceAlert.hits(before: [before], after: [after])
        XCTAssertEqual(hits.map(\.kind), [.above])
        XCTAssertEqual(hits.first?.price, Amount(cents: 1050))

        var later = after
        later.record(Price(usd: Amount(cents: 1100)), at: day(3), calendar: utc)
        XCTAssertTrue(PriceAlert.hits(before: [after], after: [later]).isEmpty, "Already above, so no repeat")

        var drop = later
        drop.record(Price(usd: Amount(cents: 450)), at: day(4), calendar: utc)
        XCTAssertEqual(PriceAlert.hits(before: [later], after: [drop]).map(\.kind), [.below])
    }

    func testCSV() {
        var item = owned("a", quantity: 2, paid: 100, prices: [(1, 1250)])
        item.card.name = "Borrowing 100,000 Arrows"
        let csv = WatchlistCSV.make(from: [item], currency: .usd)
        let lines = csv.split(separator: "\r\n")
        XCTAssertEqual(lines.count, 2)
        XCTAssertTrue(lines[1].hasPrefix("Magic,\"Borrowing 100,000 Arrows\",Commander Masters,CMM,395,Normal,,12.50,,,2,1.00,25.00,"))
        XCTAssertEqual(WatchlistCSV.plain(Amount(cents: -5)), "-0.05")
        XCTAssertEqual(WatchlistCSV.field("say \"hi\""), "\"say \"\"hi\"\"\"")
    }
}
