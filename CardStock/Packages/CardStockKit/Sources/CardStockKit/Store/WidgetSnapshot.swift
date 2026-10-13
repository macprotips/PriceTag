import Foundation

/// Just what the widget draws, worked out ahead of time.
///
/// The app writes this after every change so the widget never has to decode
/// the whole watchlist and its history.
public struct WidgetSnapshot: Codable, Equatable, Sendable {
    public struct Row: Codable, Equatable, Identifiable, Sendable {
        public var id: String
        public var game: Game
        public var name: String
        /// "DMU · Foil"
        public var subtitle: String
        public var price: Amount?
        public var dayChange: PriceChange?
        /// Change over the sparkline's range, which the widget colors it by.
        public var rangeChange: PriceChange?
        public var sparkline: [Double]
        /// File name in `SharedContainer.thumbnailsURL`.
        public var thumbnail: String?
        public var isStale: Bool

        public init(
            id: String,
            game: Game,
            name: String,
            subtitle: String,
            price: Amount?,
            dayChange: PriceChange?,
            rangeChange: PriceChange?,
            sparkline: [Double],
            thumbnail: String?,
            isStale: Bool
        ) {
            self.id = id
            self.game = game
            self.name = name
            self.subtitle = subtitle
            self.price = price
            self.dayChange = dayChange
            self.rangeChange = rangeChange
            self.sparkline = sparkline
            self.thumbnail = thumbnail
            self.isStale = isStale
        }

        /// The change the widget shows: today's if we have it, otherwise the
        /// change over the sparkline.
        public var change: PriceChange? { dayChange ?? rangeChange }
    }

    /// The value of the cards you own, for the Collection widget.
    public struct CollectionValue: Codable, Equatable, Sendable {
        public var value: Amount
        public var dayChange: PriceChange?
        public var gain: PriceChange?
        public var cardCount: Int
        public var sparkline: [Double]

        public init(value: Amount, dayChange: PriceChange?, gain: PriceChange?, cardCount: Int, sparkline: [Double]) {
            self.value = value
            self.dayChange = dayChange
            self.gain = gain
            self.cardCount = cardCount
            self.sparkline = sparkline
        }

        /// Change over the sparkline's 30 days, which colors it.
        public var rangeChange: PriceChange? {
            guard sparkline.count > 1, let first = sparkline.first, let last = sparkline.last,
                  let start = Amount(double: first), let end = Amount(double: last)
            else { return nil }
            return PriceChange(from: start, to: end)
        }
    }

    public var generated: Date
    public var currency: Currency
    public var lastRefresh: Date?
    public var rows: [Row]
    /// Nil when you haven't entered how many of any card you own.
    public var collection: CollectionValue?

    public init(generated: Date, currency: Currency, lastRefresh: Date?, rows: [Row], collection: CollectionValue? = nil) {
        self.generated = generated
        self.currency = currency
        self.lastRefresh = lastRefresh
        self.rows = rows
        self.collection = collection
    }

    public static func make(from document: WatchlistDocument, currency: Currency, now: Date = Date(), calendar: Calendar = .current) -> WidgetSnapshot {
        let rows = document.items.map { item in
            let month = item.series(in: currency, range: .month, now: now, calendar: calendar)
            return Row(
                id: item.id,
                game: item.card.game,
                name: item.card.name,
                subtitle: item.subtitle,
                price: item.currentAmount(in: currency),
                dayChange: item.dayChange(in: currency, calendar: calendar),
                rangeChange: month.change,
                sparkline: item.sparkline(in: currency, days: 30, maxPoints: 30, now: now, calendar: calendar),
                thumbnail: ThumbnailCache.fileName(for: item.card),
                isStale: item.isStale(now: now)
            )
        }
        let collection = CollectionSummary.make(from: document.items, currency: currency, days: 30, now: now, calendar: calendar)
            .map { summary in
                CollectionValue(value: summary.value, dayChange: summary.dayChange, gain: summary.gain,
                           cardCount: summary.cardCount,
                           sparkline: WatchItem.downsample(summary.history.map(\.value), to: 30))
            }
        return WidgetSnapshot(generated: now, currency: currency, lastRefresh: document.lastRefresh, rows: rows, collection: collection)
    }

    public static func load(from url: URL = SharedContainer.snapshotURL) -> WidgetSnapshot? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(WidgetSnapshot.self, from: data)
    }

    public func write(to url: URL = SharedContainer.snapshotURL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder().encode(self).write(to: url, options: .atomic)
    }

    /// Shown in the widget gallery and while the widget loads.
    public static let placeholder = WidgetSnapshot(
        generated: Date(),
        currency: .usd,
        lastRefresh: Date(),
        rows: [
            Row(id: "1", game: .magic, name: "Sheoldred, the Apocalypse", subtitle: "DMU · Foil",
                price: Amount(cents: 8420), dayChange: PriceChange(from: Amount(cents: 8223), to: Amount(cents: 8420)), rangeChange: nil,
                sparkline: [78, 79.5, 79, 81, 80.2, 82, 83.1, 82.2, 84.2], thumbnail: nil, isStale: false),
            Row(id: "2", game: .pokemon, name: "Charizard ex", subtitle: "OBF · Holo",
                price: Amount(cents: 2715), dayChange: PriceChange(from: Amount(cents: 2745), to: Amount(cents: 2715)), rangeChange: nil,
                sparkline: [29, 28.6, 28.9, 28.1, 27.8, 28, 27.4, 27.15], thumbnail: nil, isStale: false),
            Row(id: "3", game: .magic, name: "Sol Ring", subtitle: "CMM · Normal",
                price: Amount(cents: 149), dayChange: PriceChange(from: Amount(cents: 148), to: Amount(cents: 149)), rangeChange: nil,
                sparkline: [1.45, 1.44, 1.46, 1.45, 1.47, 1.48, 1.49], thumbnail: nil, isStale: false),
            Row(id: "4", game: .pokemon, name: "Pikachu ex", subtitle: "SSP · Holo",
                price: Amount(cents: 1210), dayChange: PriceChange(from: Amount(cents: 1178), to: Amount(cents: 1210)), rangeChange: nil,
                sparkline: [11.2, 11.5, 11.4, 11.8, 11.7, 12.1], thumbnail: nil, isStale: false),
            Row(id: "5", game: .magic, name: "The One Ring", subtitle: "LTR · Normal",
                price: Amount(cents: 5675), dayChange: PriceChange(from: Amount(cents: 5790), to: Amount(cents: 5675)), rangeChange: nil,
                sparkline: [60, 59.2, 58.7, 58.9, 57.5, 57, 56.75], thumbnail: nil, isStale: false),
            Row(id: "6", game: .pokemon, name: "Umbreon VMAX", subtitle: "EVS · Holo",
                price: Amount(cents: 41250), dayChange: PriceChange(from: Amount(cents: 40980), to: Amount(cents: 41250)), rangeChange: nil,
                sparkline: [398, 402, 405, 401, 407, 410, 412.5], thumbnail: nil, isStale: false),
            Row(id: "7", game: .magic, name: "Ragavan, Nimble Pilferer", subtitle: "MH2 · Normal",
                price: Amount(cents: 4410), dayChange: PriceChange(from: Amount(cents: 4410), to: Amount(cents: 4410)), rangeChange: nil,
                sparkline: [45, 44.6, 44.8, 44.2, 44.1, 44.1], thumbnail: nil, isStale: false),
            Row(id: "8", game: .pokemon, name: "Mew ex", subtitle: "MEW · Holo",
                price: Amount(cents: 615), dayChange: PriceChange(from: Amount(cents: 640), to: Amount(cents: 615)), rangeChange: nil,
                sparkline: [6.6, 6.5, 6.45, 6.3, 6.2, 6.15], thumbnail: nil, isStale: false),
        ],
        collection: CollectionValue(
            value: Amount(cents: 184_250),
            dayChange: PriceChange(from: Amount(cents: 181_020), to: Amount(cents: 184_250)),
            gain: PriceChange(from: Amount(cents: 152_000), to: Amount(cents: 184_250)),
            cardCount: 24,
            sparkline: [1712, 1725, 1719, 1748, 1760, 1752, 1781, 1795, 1790, 1810, 1842.5]
        )
    )
}
