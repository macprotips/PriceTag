@testable import CardStockKit
import XCTest

final class TCGdexTests: XCTestCase {
    func testMapsCardWithPricing() throws {
        let card = try JSONDecoder().decode(TCGdexCard.self, from: Fixture.data("tcgdex-card"))
        let quote = TCGdexProvider.quote(from: card)

        XCTAssertEqual(quote.card.id, "pokemon:swsh3-136")
        XCTAssertEqual(quote.card.name, "Furret")
        XCTAssertEqual(quote.card.setName, "Darkness Ablaze")
        XCTAssertEqual(quote.card.setCode, "SWSH3")
        XCTAssertEqual(quote.card.number, "136")
        XCTAssertEqual(quote.card.rarity, "Uncommon")
        XCTAssertEqual(quote.card.imageURL?.absoluteString, "https://assets.tcgdex.net/en/swsh/swsh3/136/high.png")
        XCTAssertEqual(quote.card.thumbnailURL?.absoluteString, "https://assets.tcgdex.net/en/swsh/swsh3/136/low.png")
        XCTAssertEqual(quote.card.finishes, [.normal, .reverseHolofoil])
        XCTAssertEqual(quote.card.links.map(\.title), ["TCGplayer", "Cardmarket"])
        XCTAssertEqual(quote.sourceUpdated, TCGdexProvider.parseDate("2025-08-28T15:22:51.000Z"), "Uses the newer market's date")

        let normal = try XCTUnwrap(quote.price(for: .normal))
        XCTAssertEqual(normal.usd, Amount(cents: 12), "TCGplayer market price")
        XCTAssertEqual(normal.usdLow, Amount(cents: 2))
        XCTAssertEqual(normal.usdHigh, Amount(cents: 299))
        XCTAssertEqual(normal.eur, Amount(cents: 8), "Cardmarket trend")
        XCTAssertEqual(normal.eurAverage7, Amount(cents: 9))
        XCTAssertEqual(normal.eurAverage30, Amount(cents: 11))

        let reverse = try XCTUnwrap(quote.price(for: .reverseHolofoil))
        XCTAssertEqual(reverse.usd, Amount(cents: 35), "Falls back to mid when there's no market price")
        XCTAssertEqual(reverse.eur, Amount(cents: 34), "Trend of 0 means no data, so it falls back to the 30-day average")
        XCTAssertEqual(reverse.eurAverage7, Amount(cents: 27))
    }

    func testCardWithoutUsablePricingUsesVariantFlags() throws {
        let card = try JSONDecoder().decode(TCGdexCard.self, from: Fixture.data("tcgdex-card-no-pricing"))
        let quote = TCGdexProvider.quote(from: card)
        XCTAssertEqual(quote.card.finishes, [.holofoil])
        XCTAssertEqual(quote.price(for: .holofoil)?.isEmpty, true)
        XCTAssertNil(quote.sourceUpdated)
    }

    func testSearchResultsAreIncomplete() throws {
        let briefs = try JSONDecoder().decode([TCGdexCardBrief].self, from: Fixture.data("tcgdex-search"))
        let quotes = briefs.map(TCGdexProvider.quote(from:))
        XCTAssertEqual(quotes.map(\.card.setCode), ["SWSH3", "SV03.5", "EX8"])
        XCTAssertEqual(quotes.map(\.card.number), ["136", "151", "102"])
        XCTAssertNil(quotes[1].card.thumbnailURL)
        XCTAssertTrue(quotes.allSatisfy { !$0.isComplete })
    }
}
