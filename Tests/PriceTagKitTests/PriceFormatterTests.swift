import XCTest
@testable import PriceTagKit

final class PriceFormatterTests: XCTestCase {
    private func text(_ input: String, _ options: PriceFormat = PriceFormat()) -> String? {
        PriceFormatter.format(input, options: options)?.text
    }

    func testAddsDollarSignAndPadsCents() {
        XCTAssertEqual(text("12.5"), "$12.50")
        XCTAssertEqual(text("$12.50"), "$12.50")
        XCTAssertEqual(text("0.71"), "$0.71")
        XCTAssertEqual(text(".71"), "$0.71")
        XCTAssertEqual(text("100"), "$100")
        XCTAssertEqual(text("  $ 100 "), "$100")
    }

    func testThousands() {
        XCTAssertEqual(text("1234"), "$1,234")
        XCTAssertEqual(text("1,234"), "$1,234")
        XCTAssertEqual(text("1234567.8"), "$1,234,567.80")
        XCTAssertEqual(text("1234", PriceFormat(groupThousands: false)), "$1234")
    }

    func testCommaDecimal() {
        XCTAssertEqual(text("12,5"), "$12.50")
        XCTAssertEqual(text("1.234,50"), "$1,234.50")
    }

    func testCentsModes() {
        XCTAssertEqual(text("100", PriceFormat(cents: .always)), "$100.00")
        XCTAssertEqual(text("12.50", PriceFormat(cents: .never)), "$13")
        XCTAssertEqual(text("12.49", PriceFormat(cents: .never)), "$12")
        XCTAssertEqual(text("0.005"), "$0.01")
    }

    func testSigns() {
        let loss = PriceFormatter.format("-110")
        XCTAssertEqual(loss?.text, "-$110")
        XCTAssertEqual(loss?.isNegative, true)
        XCTAssertEqual(text("$-110"), "-$110")
        XCTAssertEqual(text("(5)"), "-$5")
        XCTAssertEqual(text("+5"), "+$5")
        XCTAssertEqual(PriceFormatter.format("-0")?.isNegative, false)
        XCTAssertEqual(text("-0"), "$0")
    }

    func testRejectsInputWithoutDigits() {
        XCTAssertNil(PriceFormatter.format(""))
        XCTAssertNil(PriceFormatter.format("$"))
        XCTAssertNil(PriceFormatter.format("abc"))
    }

    func testLeadingZerosAndLongInput() {
        XCTAssertEqual(text("007"), "$7")
        XCTAssertEqual(text("123456789012345"), "$1,234,567,890")
    }

    func testOutputOnlyUsesFontCharacters() {
        for input in ["12.5", "-1,234.99", "+7", "(3)", "€99,90", "abc12"] {
            let output = text(input) ?? ""
            XCTAssertTrue(output.allSatisfy { MintDisplay.supportedCharacters.contains($0) }, output)
        }
    }
}
