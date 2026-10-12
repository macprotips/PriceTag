import Foundation

/// The time ranges a chart can show.
public enum ChartRange: String, CaseIterable, Identifiable, Codable, Sendable {
    case week = "1W"
    case month = "1M"
    case threeMonths = "3M"
    case sixMonths = "6M"
    case year = "1Y"
    case all = "All"

    public var id: String { rawValue }
    public var title: String { rawValue }

    /// Spoken and tooltip name, like "1 month".
    public var name: String {
        switch self {
        case .week: "1 week"
        case .month: "1 month"
        case .threeMonths: "3 months"
        case .sixMonths: "6 months"
        case .year: "1 year"
        case .all: "All time"
        }
    }

    /// The first date in the range, or nil for all time.
    public func start(endingAt end: Date, calendar: Calendar = .current) -> Date? {
        switch self {
        case .week: calendar.date(byAdding: .day, value: -7, to: end)
        case .month: calendar.date(byAdding: .month, value: -1, to: end)
        case .threeMonths: calendar.date(byAdding: .month, value: -3, to: end)
        case .sixMonths: calendar.date(byAdding: .month, value: -6, to: end)
        case .year: calendar.date(byAdding: .year, value: -1, to: end)
        case .all: nil
        }
    }
}

/// Prices over a range in one currency, ready to chart.
public struct PriceSeries: Sendable, Equatable {
    public struct Point: Hashable, Identifiable, Sendable {
        public var date: Date
        public var amount: Amount
        public var id: Date { date }
        public var value: Double { amount.double }

        public init(date: Date, amount: Amount) {
            self.date = date
            self.amount = amount
        }
    }

    /// Points inside the range, oldest first.
    public var points: [Point]
    /// The price the change is measured from: the last price before the range
    /// starts if there is one, otherwise the first price in it.
    public var baseline: Point?
    /// Where the chart's x axis should start.
    public var start: Date?

    public var latest: Point? { points.last }

    public var change: PriceChange? {
        guard let baseline, let latest, latest.date != baseline.date else { return nil }
        return PriceChange(from: baseline.amount, to: latest.amount)
    }

    public var low: Amount? { points.map(\.amount).min() }
    public var high: Amount? { points.map(\.amount).max() }

    public var isEmpty: Bool { points.isEmpty }
}

public extension WatchItem {
    /// The price now, in a currency.
    func currentAmount(in currency: Currency) -> Amount? {
        price?.amount(in: currency) ?? history.last(where: { $0.amount(in: currency) != nil })?.amount(in: currency)
    }

    /// Prices for a chart range.
    func series(in currency: Currency, range: ChartRange, now: Date = Date(), calendar: Calendar = .current) -> PriceSeries {
        let all = history.compactMap { point in
            point.amount(in: currency).map { PriceSeries.Point(date: point.date, amount: $0) }
        }
        guard let start = range.start(endingAt: now, calendar: calendar) else {
            return PriceSeries(points: all, baseline: all.first, start: all.first?.date)
        }
        let inRange = all.filter { $0.date >= start }
        let before = all.last { $0.date < start }
        return PriceSeries(points: inRange, baseline: before ?? inRange.first, start: before != nil ? start : inRange.first?.date)
    }

    /// Change since the previous day's price. Nil until there are two days of history.
    func dayChange(in currency: Currency, calendar: Calendar = .current) -> PriceChange? {
        let points = history.filter { $0.amount(in: currency) != nil }
        guard let latest = points.last, let latestAmount = latest.amount(in: currency) else { return nil }
        guard let previous = points.last(where: { !calendar.isDate($0.date, inSameDayAs: latest.date) && $0.date < latest.date }),
              let previousAmount = previous.amount(in: currency)
        else { return nil }
        return PriceChange(from: previousAmount, to: latestAmount)
    }

    /// Change since the card was added. Nil until there are two days of history.
    func changeSinceAdded(in currency: Currency) -> PriceChange? {
        let amounts = history.compactMap { $0.amount(in: currency) }
        guard amounts.count > 1, let first = amounts.first, let last = amounts.last else { return nil }
        return PriceChange(from: first, to: last)
    }

    /// The first recorded price, which is what it cost when it was added.
    func addedAmount(in currency: Currency) -> Amount? {
        history.first { $0.amount(in: currency) != nil }?.amount(in: currency)
    }

    /// Up to `maxPoints` values from the last `days` days, for a sparkline.
    func sparkline(in currency: Currency, days: Int = 30, maxPoints: Int = 30, now: Date = Date(), calendar: Calendar = .current) -> [Double] {
        let start = calendar.date(byAdding: .day, value: -days, to: now) ?? now
        let values = history
            .filter { $0.date >= start }
            .compactMap { $0.amount(in: currency)?.double }
        return Self.downsample(values, to: maxPoints)
    }

    /// Picks evenly spaced values, always keeping the first and last.
    static func downsample(_ values: [Double], to maxPoints: Int) -> [Double] {
        guard maxPoints > 1, values.count > maxPoints else { return values }
        let step = Double(values.count - 1) / Double(maxPoints - 1)
        return (0..<maxPoints).map { values[Int((Double($0) * step).rounded())] }
    }

    /// Whether the price is more than a day and a half old. Sources update
    /// daily, so anything older means refreshes have been failing.
    func isStale(now: Date = Date()) -> Bool {
        guard let lastUpdated else { return true }
        return now.timeIntervalSince(lastUpdated) > 36 * 60 * 60
    }
}
