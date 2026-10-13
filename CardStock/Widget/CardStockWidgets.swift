import AppIntents
import CardStockKit
import SwiftUI
import WidgetKit

@main
struct CardStockWidgets: WidgetBundle {
    var body: some Widget {
        CardWidget()
        WatchlistWidget()
        CollectionWidget()
    }
}

// MARK: Watchlist (medium, large)

struct WatchlistEntry: TimelineEntry {
    var date: Date
    var snapshot: WidgetSnapshot
}

struct WatchlistProvider: TimelineProvider {
    func placeholder(in context: Context) -> WatchlistEntry {
        WatchlistEntry(date: Date(), snapshot: .placeholder)
    }

    func getSnapshot(in context: Context, completion: @escaping (WatchlistEntry) -> Void) {
        if context.isPreview {
            completion(placeholder(in: context))
            return
        }
        // WidgetKit's completion handlers aren't Sendable but are safe to call from any thread.
        nonisolated(unsafe) let completion = completion
        Task {
            completion(WatchlistEntry(date: Date(), snapshot: await WidgetData.snapshot()))
        }
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<WatchlistEntry>) -> Void) {
        nonisolated(unsafe) let completion = completion
        Task {
            let entry = WatchlistEntry(date: Date(), snapshot: await WidgetData.snapshot())
            completion(Timeline(entries: [entry], policy: .after(Date().addingTimeInterval(WidgetData.reloadInterval))))
        }
    }
}

struct WatchlistWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "Watchlist", provider: WatchlistProvider()) { entry in
            WatchlistWidgetView(entry: entry)
        }
        .configurationDisplayName("Watchlist")
        .description("Prices and changes for the cards you watch.")
        .supportedFamilies([.systemMedium, .systemLarge])
    }
}

// MARK: Collection value (small, medium)

struct CollectionWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "Collection", provider: WatchlistProvider()) { entry in
            CollectionWidgetView(entry: entry)
        }
        .configurationDisplayName("Collection Value")
        .description("What the cards you own are worth, and how that's changing.")
        .supportedFamilies([.systemSmall, .systemMedium])
    }
}

// MARK: Card (small)

struct CardEntry: TimelineEntry {
    var date: Date
    var row: WidgetSnapshot.Row?
    var currency: Currency
    var lastRefresh: Date?
}

struct CardProvider: AppIntentTimelineProvider {
    func placeholder(in context: Context) -> CardEntry {
        let snapshot = WidgetSnapshot.placeholder
        return CardEntry(date: Date(), row: snapshot.rows.first, currency: snapshot.currency, lastRefresh: snapshot.lastRefresh)
    }

    func snapshot(for configuration: SelectCardIntent, in context: Context) async -> CardEntry {
        if context.isPreview { return placeholder(in: context) }
        return makeEntry(for: configuration, snapshot: await WidgetData.snapshot())
    }

    func timeline(for configuration: SelectCardIntent, in context: Context) async -> Timeline<CardEntry> {
        let entry = makeEntry(for: configuration, snapshot: await WidgetData.snapshot())
        return Timeline(entries: [entry], policy: .after(Date().addingTimeInterval(WidgetData.reloadInterval)))
    }

    /// The chosen card, or the one that moved most today.
    private func makeEntry(for configuration: SelectCardIntent, snapshot: WidgetSnapshot) -> CardEntry {
        let chosen = configuration.card.flatMap { card in snapshot.rows.first { $0.id == card.id } }
        let mover = snapshot.rows.max { abs($0.change?.fraction ?? 0) < abs($1.change?.fraction ?? 0) }
        return CardEntry(date: Date(), row: chosen ?? mover, currency: snapshot.currency, lastRefresh: snapshot.lastRefresh)
    }
}

struct CardWidget: Widget {
    var body: some WidgetConfiguration {
        AppIntentConfiguration(kind: "Card", intent: SelectCardIntent.self, provider: CardProvider()) { entry in
            CardWidgetView(entry: entry)
        }
        .configurationDisplayName("Card")
        .description("One card's price and how it's moving.")
        .supportedFamilies([.systemSmall])
    }
}
