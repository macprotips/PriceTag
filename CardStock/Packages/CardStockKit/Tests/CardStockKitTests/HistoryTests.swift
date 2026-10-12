@testable import CardStockKit
import XCTest

final class HistoryTests: XCTestCase {
    private func makeItem(_ prices: [(day: Int, cents: Int)]) -> WatchItem {
        var item = WatchItem(card: sampleCard(), finish: .nonfoil, added: day(prices.first?.day ?? 1))
        for price in prices {
            item.record(Price(usd: Amount(cents: price.cents)), at: day(price.day), calendar: utc)
        }
        return item
    }

    func testKeepsOnePointPerDay() {
        var item = makeItem([(1, 100), (2, 110)])
        item.record(Price(usd: Amount(cents: 120)), at: day(2, hour: 18), calendar: utc)
        XCTAssertEqual(item.history.map(\.usd), [Amount(cents: 100), Amount(cents: 120)])
        XCTAssertEqual(item.price?.usd, Amount(cents: 120))
    }

    func testKeepsHistorySortedWhenAPointArrivesLate() {
        var item = makeItem([(1, 100), (3, 130)])
        item.record(Price(usd: Amount(cents: 115)), at: day(2), calendar: utc)
        XCTAssertEqual(item.history.map(\.usd), [Amount(cents: 100), Amount(cents: 115), Amount(cents: 130)])
    }

    func testEmptyPriceIsNotRecorded() {
        var item = makeItem([(1, 100)])
        item.record(Price(), at: day(2), calendar: utc)
        XCTAssertEqual(item.history.count, 1)
    }

    func testDayChangeNeedsTwoDays() {
        XCTAssertNil(makeItem([(1, 100)]).dayChange(in: .usd, calendar: utc))

        let change = makeItem([(1, 100), (2, 90), (5, 99)]).dayChange(in: .usd, calendar: utc)
        XCTAssertEqual(change?.amount, Amount(cents: 9), "Compares with the most recent earlier day, even with gaps")
        XCTAssertEqual(change?.fraction ?? 0, 0.1, accuracy: 0.0001)
    }

    func testDayChangeIgnoresOtherCurrency() {
        var item = makeItem([(1, 100)])
        item.record(Price(eur: Amount(cents: 80)), at: day(2), calendar: utc)
        XCTAssertNil(item.dayChange(in: .usd, calendar: utc), "Day 2 has no dollar price")
        XCTAssertNil(item.dayChange(in: .eur, calendar: utc))
    }

    func testSeriesForRange() {
        let item = makeItem([(1, 100), (20, 150), (25, 160), (30, 140)])
        let week = item.series(in: .usd, range: .week, now: day(30), calendar: utc)

        XCTAssertEqual(week.points.map(\.amount.cents), [160, 140])
        XCTAssertEqual(week.baseline?.amount, Amount(cents: 150), "Measured from the last price before the range")
        XCTAssertEqual(week.start, day(23))
        XCTAssertEqual(week.change?.amount, Amount(cents: -10))
        XCTAssertEqual(week.low, Amount(cents: 140))
        XCTAssertEqual(week.high, Amount(cents: 160))

        let all = item.series(in: .usd, range: .all, now: day(30), calendar: utc)
        XCTAssertEqual(all.points.count, 4)
        XCTAssertEqual(all.baseline?.amount, Amount(cents: 100))
        XCTAssertEqual(all.change?.amount, Amount(cents: 40))
    }

    func testSeriesWithoutEarlierHistoryStartsAtFirstPoint() {
        let series = makeItem([(28, 100), (30, 120)]).series(in: .usd, range: .month, now: day(30), calendar: utc)
        XCTAssertEqual(series.start, day(28))
        XCTAssertEqual(series.baseline?.amount, Amount(cents: 100))
        XCTAssertEqual(series.change?.amount, Amount(cents: 20))
    }

    func testSinglePointHasNoChange() {
        let series = makeItem([(30, 100)]).series(in: .usd, range: .month, now: day(30), calendar: utc)
        XCTAssertEqual(series.points.count, 1)
        XCTAssertNil(series.change)
    }

    func testChangeSinceAdded() {
        let item = makeItem([(1, 200), (10, 150), (20, 250)])
        XCTAssertEqual(item.addedAmount(in: .usd), Amount(cents: 200))
        XCTAssertEqual(item.changeSinceAdded(in: .usd)?.amount, Amount(cents: 50))
    }

    func testSparkline() {
        let prices = (1...31).map { (day: $0, cents: $0 * 100) }
        let line = makeItem(prices).sparkline(in: .usd, days: 30, maxPoints: 10, now: day(31), calendar: utc)
        XCTAssertEqual(line.count, 10)
        XCTAssertEqual(line.first, 1, "Only the last 30 days")
        XCTAssertEqual(line.last, 31, "Always ends on the latest price")
    }

    func testDownsampleLeavesShortSeriesAlone() {
        XCTAssertEqual(WatchItem.downsample([1, 2, 3], to: 10), [1, 2, 3])
        XCTAssertEqual(WatchItem.downsample([1, 2, 3, 4, 5], to: 3), [1, 3, 5])
    }

    func testStale() {
        var item = makeItem([(1, 100)])
        XCTAssertTrue(item.isStale(now: day(1)), "Never updated")
        item.lastUpdated = day(1)
        XCTAssertFalse(item.isStale(now: day(2)))
        XCTAssertTrue(item.isStale(now: day(3)))
    }

    func testApplyingQuoteWithoutPriceForFinishKeepsHistory() {
        var item = makeItem([(1, 100)])
        let quote = CardQuote(card: sampleCard(), prices: [.foil: Price(usd: Amount(cents: 500))])
        item.apply(quote, at: day(2), calendar: utc)
        XCTAssertNil(item.price)
        XCTAssertNotNil(item.lastError)
        XCTAssertEqual(item.history.count, 1)
        XCTAssertEqual(item.currentAmount(in: .usd), Amount(cents: 100), "Falls back to the last known price")
    }
}
