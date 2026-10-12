@testable import CardStockKit
import XCTest

final class FormatTests: XCTestCase {
    private let us = Locale(identifier: "en_US")

    func testPrice() {
        XCTAssertEqual(PriceFormat.price(Amount(cents: 8420), .usd, locale: us), "$84.20")
        XCTAssertEqual(PriceFormat.price(Amount(cents: 123_456), .usd, locale: us), "$1,234.56")
        XCTAssertEqual(PriceFormat.price(nil, .usd, locale: us), "—")
    }

    func testChangeUsesRealSigns() {
        XCTAssertEqual(PriceFormat.change(Amount(cents: 197), .usd, locale: us), "+$1.97")
        XCTAssertEqual(PriceFormat.change(Amount(cents: -30), .usd, locale: us), "\u{2212}$0.30")
        XCTAssertEqual(PriceFormat.percent(0.024, locale: us), "+2.4%")
        XCTAssertEqual(PriceFormat.percent(-0.011, locale: us), "\u{2212}1.1%")
    }

    func testChangeFallsBackToAmount() {
        let change = PriceChange(amount: Amount(cents: 100), fraction: nil)
        XCTAssertEqual(PriceFormat.change(change, .usd, locale: us), "+$1.00")
        XCTAssertEqual(PriceFormat.change(nil, .usd, locale: us), "—")
    }

    func testSpoken() {
        XCTAssertEqual(PriceFormat.spokenChange(PriceChange(from: Amount(cents: 100), to: Amount(cents: 100)), .usd), "unchanged")
        XCTAssertEqual(PriceFormat.spokenChange(nil, .usd), "no change yet")
    }
}
