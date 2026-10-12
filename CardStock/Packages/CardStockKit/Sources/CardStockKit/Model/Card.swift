import Foundation

/// One printing of a card, such as Sol Ring from Commander Masters, #395.
public struct Card: Identifiable, Hashable, Codable, Sendable {
    public var game: Game
    /// The card's ID at its source: a Scryfall UUID or a TCGdex ID like `swsh3-136`.
    public var sourceID: String
    public var name: String
    public var setName: String
    /// Short set code, like `DMU` or `SWSH3`.
    public var setCode: String
    public var number: String
    public var rarity: String?
    /// Full size image, for the detail view.
    public var imageURL: URL?
    /// Small image, for lists and widgets.
    public var thumbnailURL: URL?
    /// Finishes this printing comes in, most common first.
    public var finishes: [Finish]
    /// Places to buy it.
    public var links: [PurchaseLink]
    /// The card's page at its source.
    public var pageURL: URL?

    public init(
        game: Game,
        sourceID: String,
        name: String,
        setName: String,
        setCode: String,
        number: String,
        rarity: String? = nil,
        imageURL: URL? = nil,
        thumbnailURL: URL? = nil,
        finishes: [Finish] = [],
        links: [PurchaseLink] = [],
        pageURL: URL? = nil
    ) {
        self.game = game
        self.sourceID = sourceID
        self.name = name
        self.setName = setName
        self.setCode = setCode
        self.number = number
        self.rarity = rarity
        self.imageURL = imageURL
        self.thumbnailURL = thumbnailURL
        self.finishes = finishes
        self.links = links
        self.pageURL = pageURL
    }

    public var id: String { "\(game.rawValue):\(sourceID)" }
}

/// A place to buy a card.
public struct PurchaseLink: Hashable, Codable, Sendable {
    public var title: String
    public var url: URL

    public init(title: String, url: URL) {
        self.title = title
        self.url = url
    }
}

/// The price of one finish of a card, in both currencies.
public struct Price: Hashable, Codable, Sendable {
    /// TCGplayer market price.
    public var usd: Amount?
    /// Cardmarket trend price.
    public var eur: Amount?
    /// TCGplayer low and high listings, when the source has them.
    public var usdLow: Amount?
    public var usdHigh: Amount?
    /// Cardmarket 7 and 30-day average sale price, when the source has them.
    public var eurAverage7: Amount?
    public var eurAverage30: Amount?

    public init(
        usd: Amount? = nil,
        eur: Amount? = nil,
        usdLow: Amount? = nil,
        usdHigh: Amount? = nil,
        eurAverage7: Amount? = nil,
        eurAverage30: Amount? = nil
    ) {
        self.usd = usd
        self.eur = eur
        self.usdLow = usdLow
        self.usdHigh = usdHigh
        self.eurAverage7 = eurAverage7
        self.eurAverage30 = eurAverage30
    }

    public func amount(in currency: Currency) -> Amount? {
        switch currency {
        case .usd: usd
        case .eur: eur
        }
    }

    public var isEmpty: Bool { usd == nil && eur == nil }
}

/// What a source said about a card: its details and today's prices.
public struct CardQuote: Identifiable, Hashable, Sendable {
    public var card: Card
    public var prices: [Finish: Price]
    /// When the source last updated these prices, if it says.
    public var sourceUpdated: Date?
    /// False for a search result that only has the basics. Load the full
    /// quote with `PriceProvider.quote(for:)` before showing prices.
    public var isComplete: Bool

    public init(card: Card, prices: [Finish: Price] = [:], sourceUpdated: Date? = nil, isComplete: Bool = true) {
        self.card = card
        self.prices = prices
        self.sourceUpdated = sourceUpdated
        self.isComplete = isComplete
    }

    public var id: String { card.id }

    public func price(for finish: Finish) -> Price? { prices[finish] }

    /// The finish to suggest when adding the card: the first with a price in
    /// this currency, otherwise the first one.
    public func suggestedFinish(for currency: Currency) -> Finish? {
        card.finishes.first { prices[$0]?.amount(in: currency) != nil } ?? card.finishes.first
    }
}
