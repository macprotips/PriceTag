import Foundation

/// A card on the watchlist: one printing in one finish, with its price history.
public struct WatchItem: Identifiable, Hashable, Codable, Sendable {
    public var card: Card
    public var finish: Finish
    public var added: Date
    /// The latest price for this finish.
    public var price: Price?
    /// One point per day, oldest first.
    public var history: [PricePoint]
    /// When we last heard from the source about this card.
    public var lastUpdated: Date?
    /// When the source says it last updated the price.
    public var sourceUpdated: Date?
    /// Why the last refresh failed, if it did. Cleared on success.
    public var lastError: String?

    public init(
        card: Card,
        finish: Finish,
        added: Date = Date(),
        price: Price? = nil,
        history: [PricePoint] = [],
        lastUpdated: Date? = nil,
        sourceUpdated: Date? = nil,
        lastError: String? = nil
    ) {
        self.card = card
        self.finish = finish
        self.added = added
        self.price = price
        self.history = history
        self.lastUpdated = lastUpdated
        self.sourceUpdated = sourceUpdated
        self.lastError = lastError
    }

    /// Starts watching a card, recording its current price as the first point.
    public init(quote: CardQuote, finish: Finish, at date: Date = Date(), calendar: Calendar = .current) {
        self.init(card: quote.card, finish: finish, added: date)
        apply(quote, at: date, calendar: calendar)
    }

    public static func id(cardID: String, finish: Finish) -> String { "\(cardID):\(finish.rawValue)" }

    public var id: String { Self.id(cardID: card.id, finish: finish) }

    /// "DMU · Foil"
    public var subtitle: String { "\(card.setCode) · \(finish.name)" }

    /// Updates the card details and records today's price from a fresh quote.
    public mutating func apply(_ quote: CardQuote, at date: Date, calendar: Calendar = .current) {
        card = quote.card
        lastUpdated = date
        sourceUpdated = quote.sourceUpdated
        if let price = quote.price(for: finish), !price.isEmpty {
            record(price, at: date, calendar: calendar)
            lastError = nil
        } else {
            price = nil
            lastError = "No \(finish.name.lowercased()) price right now."
        }
    }

    /// Records a price. There's one history point per day, so a second price
    /// on the same day replaces the first.
    public mutating func record(_ price: Price, at date: Date, calendar: Calendar = .current) {
        self.price = price
        guard !price.isEmpty else { return }
        let point = PricePoint(date: date, usd: price.usd, eur: price.eur)
        if let index = history.lastIndex(where: { calendar.isDate($0.date, inSameDayAs: date) }) {
            history[index] = point
        } else {
            history.append(point)
            if history.count > 1, history[history.count - 2].date > date {
                history.sort { $0.date < $1.date }
            }
        }
    }
}

/// A price on a given day.
public struct PricePoint: Hashable, Codable, Sendable {
    public var date: Date
    public var usd: Amount?
    public var eur: Amount?

    public init(date: Date, usd: Amount? = nil, eur: Amount? = nil) {
        self.date = date
        self.usd = usd
        self.eur = eur
    }

    public func amount(in currency: Currency) -> Amount? {
        switch currency {
        case .usd: usd
        case .eur: eur
        }
    }
}
