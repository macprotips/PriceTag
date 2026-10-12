@testable import CardStockKit
import XCTest

final class ScryfallTests: XCTestCase {
    func testMapsCard() throws {
        let card = try ScryfallProvider.decoder.decode(ScryfallCard.self, from: Fixture.data("scryfall-card"))
        let quote = ScryfallProvider.quote(from: card)

        XCTAssertEqual(quote.card.id, "magic:4e4fb50c-a81f-44d3-93c5-fa9a0b37f617")
        XCTAssertEqual(quote.card.name, "Sheoldred, the Apocalypse")
        XCTAssertEqual(quote.card.setName, "Dominaria United")
        XCTAssertEqual(quote.card.setCode, "DMU")
        XCTAssertEqual(quote.card.number, "107")
        XCTAssertEqual(quote.card.rarity, "Mythic")
        XCTAssertEqual(quote.card.finishes, [.nonfoil, .foil])
        XCTAssertEqual(quote.card.imageURL?.absoluteString, "https://cards.scryfall.io/large/front/4/e/4e4fb50c.jpg")
        XCTAssertEqual(quote.card.thumbnailURL?.absoluteString, "https://cards.scryfall.io/small/front/4/e/4e4fb50c.jpg")
        XCTAssertEqual(quote.card.pageURL?.absoluteString, "https://scryfall.com/card/dmu/107/sheoldred-the-apocalypse")
        XCTAssertEqual(quote.card.links.map(\.title), ["TCGplayer", "Cardmarket"], "MTGO stores are left out")
        XCTAssertTrue(quote.isComplete)

        XCTAssertEqual(quote.price(for: .nonfoil), Price(usd: Amount(cents: 7912), eur: Amount(cents: 7005)))
        XCTAssertEqual(quote.price(for: .foil), Price(usd: Amount(cents: 8420), eur: Amount(cents: 7130)))
        XCTAssertNil(quote.price(for: .etched))
    }

    func testMapsDoubleFacedCardsAndMissingCards() throws {
        let collection = try ScryfallProvider.decoder.decode(ScryfallCollection.self, from: Fixture.data("scryfall-collection"))
        XCTAssertEqual(collection.notFound?.first?.id, "00000000-0000-0000-0000-000000000000")

        let quote = ScryfallProvider.quote(from: try XCTUnwrap(collection.data.first))
        XCTAssertEqual(quote.card.imageURL?.absoluteString, "https://cards.scryfall.io/large/front/b/2/b2c6aa39.jpg",
                       "Double-faced cards use the front face's image")
        XCTAssertEqual(quote.price(for: .nonfoil), Price(usd: Amount(cents: 71), eur: nil))
        XCTAssertEqual(quote.price(for: .foil), Price(usd: Amount(cents: 610), eur: Amount(cents: 495)))
        XCTAssertEqual(quote.suggestedFinish(for: .eur), .foil, "Suggests the first finish with a euro price")
        XCTAssertEqual(quote.suggestedFinish(for: .usd), .nonfoil)
    }
}
