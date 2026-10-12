import AppKit
import CardStockKit
import SwiftUI
import WidgetKit

// MARK: Small

struct CardWidgetView: View {
    var entry: CardEntry

    var body: some View {
        Group {
            if let row = entry.row {
                content(row)
                    .widgetURL(DeepLink.card(row.id))
            } else {
                EmptyWidgetMessage()
            }
        }
        .containerBackground(.fill.tertiary, for: .widget)
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
                Thumbnail(row: row, width: 26)
            }

            Spacer(minLength: 6)

            Sparkline(values: row.sparkline, direction: row.rangeChange.direction)
                .frame(height: 26)

            Spacer(minLength: 6)

            Text(PriceFormat.price(row.price, entry.currency))
                .font(.title2.weight(.semibold))
                .monospacedDigit()
                .minimumScaleFactor(0.6)
                .lineLimit(1)
            Text(PriceFormat.change(row.change, entry.currency))
                .font(.subheadline.weight(.medium))
                .monospacedDigit()
                .foregroundStyle(row.change.direction.color)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(row.name), \(row.subtitle), \(PriceFormat.price(row.price, entry.currency)), \(PriceFormat.spokenChange(row.change, entry.currency))")
    }
}

// MARK: Medium and large

struct WatchlistWidgetView: View {
    @Environment(\.widgetFamily) private var family
    var entry: WatchlistEntry

    private var rowCount: Int { family == .systemLarge ? 7 : 3 }

    var body: some View {
        Group {
            if entry.snapshot.rows.isEmpty {
                EmptyWidgetMessage()
            } else {
                VStack(alignment: .leading, spacing: 0) {
                    if family == .systemLarge {
                        header
                            .padding(.bottom, 6)
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
        .containerBackground(.fill.tertiary, for: .widget)
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("Watchlist")
                .font(.headline)
            Spacer()
            if let lastRefresh = entry.snapshot.lastRefresh {
                Text(lastRefresh, style: .relative)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.trailing)
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
                ChangeBadge(change: row.change, currency: currency, minWidth: 58, font: .caption.weight(.medium))
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(row.name), \(row.subtitle), \(PriceFormat.price(row.price, currency)), \(PriceFormat.spokenChange(row.change, currency))")
    }
}

// MARK: Shared pieces

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
        }
    }
}

private struct EmptyWidgetMessage: View {
    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: "rectangle.portrait.on.rectangle.portrait")
                .font(.title2)
                .foregroundStyle(.secondary)
            Text("No Cards")
                .font(.headline)
            Text("Add cards in CardStock.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .multilineTextAlignment(.center)
    }
}
