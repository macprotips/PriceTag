import Foundation

/// The value of the cards you own: every watchlist item with a holding.
public struct CollectionSummary: Equatable, Sendable {
    /// Total value now.
    public var value: Amount
    /// Change in total value since the previous day.
    public var dayChange: PriceChange?
    /// Value now against what you paid, for cards where you entered a price paid.
    public var gain: PriceChange?
    /// Number of copies owned.
    public var cardCount: Int
    /// Total value at the end of each day, oldest first.
    public var history: [PriceSeries.Point]

    public init(value: Amount, dayChange: PriceChange?, gain: PriceChange?, cardCount: Int, history: [PriceSeries.Point]) {
        self.value = value
        self.dayChange = dayChange
        self.gain = gain
        self.cardCount = cardCount
        self.history = history
    }

    /// Nil when you haven't said you own any cards.
    public static func make(
        from items: [WatchItem],
        currency: Currency,
        days: Int = 30,
        now: Date = Date(),
        calendar: Calendar = .current
    ) -> CollectionSummary? {
        let owned = items.filter { ($0.holding?.quantity ?? 0) > 0 }
        guard !owned.isEmpty else { return nil }

        var value = Amount.zero
        var paid = Amount.zero
        var valueOfPaidCards = Amount.zero
        var count = 0
        for item in owned {
            let quantity = item.holding?.quantity ?? 0
            count += quantity
            guard let price = item.currentAmount(in: currency) else { continue }
            let total = Amount(cents: price.cents * quantity)
            value = value + total
            if let holding = item.holding, holding.currency == currency, let each = holding.pricePaid {
                paid = paid + Amount(cents: each.cents * quantity)
                valueOfPaidCards = valueOfPaidCards + total
            }
        }

        let history = Self.history(of: owned, currency: currency, days: days, now: now, calendar: calendar)
        var dayChange: PriceChange?
        if history.count > 1 {
            dayChange = PriceChange(from: history[history.count - 2].amount, to: history[history.count - 1].amount)
        }
        let gain = paid.isZero ? nil : PriceChange(from: paid, to: valueOfPaidCards)
        return CollectionSummary(value: value, dayChange: dayChange, gain: gain, cardCount: count, history: history)
    }

    /// Total value at the end of each of the last `days` days. Each card counts
    /// at its last known price on or before that day. Days before any card had
    /// a price are left out.
    static func history(of items: [WatchItem], currency: Currency, days: Int, now: Date, calendar: Calendar) -> [PriceSeries.Point] {
        let today = calendar.startOfDay(for: now)
        var points: [PriceSeries.Point] = []
        for offset in stride(from: days - 1, through: 0, by: -1) {
            guard let dayStart = calendar.date(byAdding: .day, value: -offset, to: today),
                  let dayEnd = calendar.date(byAdding: .day, value: 1, to: dayStart)
            else { continue }
            var total = 0
            var anyPriced = false
            for item in items {
                guard let point = item.history.last(where: { $0.date < dayEnd && $0.amount(in: currency) != nil }),
                      let amount = point.amount(in: currency)
                else { continue }
                total += amount.cents * (item.holding?.quantity ?? 0)
                anyPriced = true
            }
            if anyPriced {
                points.append(PriceSeries.Point(date: offset == 0 ? now : dayEnd.addingTimeInterval(-1), amount: Amount(cents: total)))
            }
        }
        return points
    }
}

/// A price alert that went off.
public struct AlertHit: Equatable, Sendable {
    public enum Kind: Sendable { case above, below }

    public var itemID: String
    public var cardName: String
    public var subtitle: String
    public var kind: Kind
    public var level: Amount
    public var price: Amount
    public var currency: Currency
}

public extension PriceAlert {
    /// Alerts crossed between the prices before and after a refresh. An alert
    /// only goes off when the price moves across its level, so it doesn't
    /// repeat on every refresh while the price stays there.
    static func hits(before: [WatchItem], after: [WatchItem]) -> [AlertHit] {
        let previous = Dictionary(before.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        var hits: [AlertHit] = []
        for item in after {
            guard let alert = item.alert, !alert.isEmpty,
                  let old = previous[item.id]?.currentAmount(in: alert.currency),
                  let new = item.price?.amount(in: alert.currency)
            else { continue }
            func hit(_ kind: AlertHit.Kind, _ level: Amount) -> AlertHit {
                AlertHit(itemID: item.id, cardName: item.card.name, subtitle: item.subtitle,
                         kind: kind, level: level, price: new, currency: alert.currency)
            }
            if let above = alert.above, old < above, new >= above { hits.append(hit(.above, above)) }
            if let below = alert.below, old > below, new <= below { hits.append(hit(.below, below)) }
        }
        return hits
    }
}
