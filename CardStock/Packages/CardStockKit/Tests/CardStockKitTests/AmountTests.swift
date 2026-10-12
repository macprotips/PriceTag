@testable import CardStockKit
import XCTest

final class AmountTests: XCTestCase {
    func testParsesStrings() {
        XCTAssertEqual(Amount(string: "79.12"), Amount(cents: 7912))
        XCTAssertEqual(Amount(string: "0.07"), Amount(cents: 7))
        XCTAssertEqual(Amount(string: "12.5"), Amount(cents: 1250))
        XCTAssertEqual(Amount(string: "1234"), Amount(cents: 123_400))
        XCTAssertEqual(Amount(string: "0.005"), Amount(cents: 1), "Rounds half up to the nearest cent")
        XCTAssertNil(Amount(string: ""))
        XCTAssertNil(Amount(string: "n/a"))
    }

    func testRoundsDoubles() {
        XCTAssertEqual(Amount(double: 0.1 + 0.2), Amount(cents: 30))
        XCTAssertEqual(Amount(double: 27.149999), Amount(cents: 2715))
        XCTAssertNil(Amount(double: nil))
        XCTAssertNil(Amount(double: .nan))
    }

    func testEncodesAsWholeCents() throws {
        let data = try JSONEncoder().encode([Amount(cents: 10)])
        XCTAssertEqual(String(decoding: data, as: UTF8.self), "[10]")
        XCTAssertEqual(try JSONDecoder().decode([Amount].self, from: data), [Amount(cents: 10)])
    }

    func testChange() {
        let up = PriceChange(from: Amount(cents: 8223), to: Amount(cents: 8420))
        XCTAssertEqual(up.amount, Amount(cents: 197))
        XCTAssertEqual(up.fraction!, 0.02396, accuracy: 0.00001)
        XCTAssertEqual(up.direction, .up)

        XCTAssertEqual(PriceChange(from: Amount(cents: 100), to: Amount(cents: 90)).direction, .down)
        XCTAssertEqual(PriceChange(from: Amount(cents: 100), to: Amount(cents: 100)).direction, .flat)
        XCTAssertNil(PriceChange(from: .zero, to: Amount(cents: 100)).fraction)
    }
}
