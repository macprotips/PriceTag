import AppKit
import CardStockKit
import SwiftUI
import WidgetKit

// MARK: Card (small)

struct CardWidgetView: View {
    var entry: CardEntry

    var body: some View {
        Group {
            if let row = entry.row {
                content(row)
                    .widgetURL(DeepLink.card(row.id))
            } else {
                EmptyWidgetMessage(title: "No Cards", message: "Add cards in CardStock to see them here.")
            }
        }
        .containerBackground(.background, for: .widget)
    }

    private func content(_ row: WidgetSnapshot.Row) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .top, spacing: 8) {
                VStack(alignment: .leading, spacing: 1) {
                    Text(row.name)
                        .font(.headline)
                        .lineLimit(2)
                    Text(row.subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 0)
                Thumbnail(row: row, width: 24)
            }

            Spacer(minLength: 6)

            Sparkline(values: row.sparkline, direction: row.rangeChange.direction)
                .frame(height: 28)

            Spacer(minLength: 6)

            Text(PriceFormat.price(row.price, entry.currency))
                .font(.title2.weight(.semibold))
                .monospacedDigit()
                .minimumScaleFactor(0.6)
                .lineLimit(1)
                .foregroundStyle(row.isStale ? .secondary : .primary)
                .contentTransition(.numericText())
            ChangeText(change: row.change, currency: entry.currency)
                .font(.subheadline.weight(.semibold))
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(spoken(row, currency: entry.currency))
    }
}

// MARK: Watchlist (medium, large)

struct WatchlistWidgetView: View {
    @Environment(\.widgetFamily) private var family
    var entry: WatchlistEntry

    private var rowCount: Int { family == .systemLarge ? 7 : 3 }

    var body: some View {
        Group {
            if entry.snapshot.rows.isEmpty {
                EmptyWidgetMessage(title: "No Cards", message: "Add cards in CardStock to see them here.")
            } else {
                VStack(alignment: .leading, spacing: 0) {
                    if family == .systemLarge {
                        header
                            .padding(.bottom, 4)
                    }
                    let rows = Array(entry.snapshot.rows.prefix(rowCount))
                    ForEach(Array(rows.enumerated()), id: \.element.id) { index, row in
                        if index > 0 { Divider() }
                        Link(destination: DeepLink.card(row.id)) {
                            WidgetRow(row: row, currency: entry.snapshot.currency)
                        }
                        .frame(maxHeight: .infinity)
                    }
                    if rows.count < rowCount { Spacer(minLength: 0) }
                }
            }
        }
        .containerBackground(.background, for: .widget)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("Watchlist")
                .font(.headline)
            Spacer()
            if let lastRefresh = entry.snapshot.lastRefresh {
                Text("Updated \(lastRefresh.formatted(date: .omitted, time: .shortened))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct WidgetRow: View {
    var row: WidgetSnapshot.Row
    var currency: Currency

    var body: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 1) {
                Text(row.name)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                Text(row.subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 4)
            Sparkline(values: row.sparkline, direction: row.rangeChange.direction)
                .frame(width: 40, height: 16)
            VStack(alignment: .trailing, spacing: 2) {
                Text(PriceFormat.price(row.price, currency))
                    .font(.subheadline.weight(.semibold))
                    .monospacedDigit()
                    .lineLimit(1)
                    .foregroundStyle(row.isStale ? .secondary : .primary)
                ChangeBadge(change: row.change, currency: currency, minWidth: 58, font: .caption.weight(.semibold))
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(spoken(row, currency: currency))
    }
}

// MARK: Collection value (small, medium)

struct CollectionWidgetView: View {
    @Environment(\.widgetFamily) private var family
    var entry: WatchlistEntry

    var body: some View {
        Group {
            if let collection = entry.snapshot.collection {
                if family == .systemMedium {
                    medium(collection)
                } else {
                    small(collection)
                }
            } else {
                EmptyWidgetMessage(title: "No Collection",
                                   message: "In CardStock, enter how many of a card you own to see your collection’s value.")
            }
        }
        .containerBackground(.background, for: .widget)
    }

    private var currency: Currency { entry.snapshot.currency }

    private func small(_ collection: WidgetSnapshot.CollectionValue) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Collection")
                .font(.headline)
            Text(cardCount(collection))
                .font(.caption)
                .foregroundStyle(.secondary)
            Spacer(minLength: 6)
            Sparkline(values: collection.sparkline, direction: collection.rangeChange.direction)
                .frame(height: 28)
            Spacer(minLength: 6)
            value(collection)
                .font(.title2.weight(.semibold))
            ChangeText(change: collection.dayChange, currency: currency, suffix: "today")
                .font(.subheadline.weight(.semibold))
        }
        .accessibilityElement(children: .combine)
    }

    private func medium(_ collection: WidgetSnapshot.CollectionValue) -> some View {
        HStack(alignment: .top, spacing: 16) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Collection")
                    .font(.headline)
                Text(cardCount(collection))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer(minLength: 6)
                value(collection)
                    .font(.title.weight(.semibold))
                ChangeText(change: collection.dayChange, currency: currency, suffix: "today")
                    .font(.subheadline.weight(.semibold))
                if let gain = collection.gain {
                    ChangeText(change: gain, currency: currency, suffix: "since bought")
                        .font(.caption.weight(.medium))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            VStack(alignment: .trailing, spacing: 4) {
                Text("30 days")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Sparkline(values: collection.sparkline, direction: collection.rangeChange.direction, lineWidth: 2)
            }
            .frame(maxWidth: .infinity)
        }
        .accessibilityElement(children: .combine)
    }

    private func value(_ collection: WidgetSnapshot.CollectionValue) -> some View {
        Text(PriceFormat.price(collection.value, currency))
            .monospacedDigit()
            .minimumScaleFactor(0.6)
            .lineLimit(1)
            .contentTransition(.numericText())
    }

    private func cardCount(_ collection: WidgetSnapshot.CollectionValue) -> String {
        collection.cardCount == 1 ? "1 card" : "\(collection.cardCount) cards"
    }
}

// MARK: Shared pieces

/// "+$1.97 (+2.4%)" in green or red. Uses the sign alone when macOS tints the widget.
private struct ChangeText: View {
    @Environment(\.widgetRenderingMode) private var renderingMode
    var change: PriceChange?
    var currency: Currency
    var suffix: String?

    var body: some View {
        HStack(spacing: 4) {
            Text(text)
                .monospacedDigit()
                .foregroundStyle(renderingMode == .fullColor ? change.direction.color : .primary)
            if let suffix, change != nil {
                Text(suffix).foregroundStyle(.secondary)
            }
        }
        .lineLimit(1)
        .minimumScaleFactor(0.8)
    }

    private var text: String {
        guard let change else { return "—" }
        let amount = PriceFormat.change(change.amount, currency)
        guard let fraction = change.fraction else { return amount }
        return "\(amount) (\(PriceFormat.percent(fraction)))"
    }
}

/// A card image saved by the app. Widgets can't download while drawing.
private struct Thumbnail: View {
    var row: WidgetSnapshot.Row
    var width: CGFloat

    var body: some View {
        if let url = WidgetData.thumbnailURL(row), let image = NSImage(contentsOf: url) {
            Image(nsImage: image)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(width: width)
                .clipShape(RoundedRectangle(cornerRadius: width * 0.047, style: .continuous))
                .widgetAccentable(false)
        }
    }
}

private struct EmptyWidgetMessage: View {
    var title: String
    var message: String

    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: "rectangle.portrait.on.rectangle.portrait")
                .font(.title2)
                .foregroundStyle(.secondary)
            Text(title)
                .font(.headline)
            Text(message)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .multilineTextAlignment(.center)
    }
}

private func spoken(_ row: WidgetSnapshot.Row, currency: Currency) -> String {
    "\(row.name), \(row.subtitle), \(PriceFormat.price(row.price, currency)), \(PriceFormat.spokenChange(row.change, currency))"
}
