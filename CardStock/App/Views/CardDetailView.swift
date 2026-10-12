import CardStockKit
import SwiftUI

struct CardDetailView: View {
    @Environment(AppModel.self) private var model
    var item: WatchItem

    /// The point under the pointer in the chart, if any.
    @State private var hovered: PriceSeries.Point?
    @State private var showingLargeImage = false

    private static let artworkWidth: CGFloat = 250

    var body: some View {
        @Bindable var model = model
        let currency = model.currency
        let series = item.series(in: currency, range: model.chartRange)

        let info = VStack(alignment: .leading, spacing: 22) {
            header
            priceHeader(series: series, currency: currency)

            Picker("Range", selection: $model.chartRange) {
                ForEach(ChartRange.allCases) { range in
                    Text(range.title).tag(range).help(range.name.capitalized)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 360)

            PriceChart(series: series, currency: currency, historyStart: item.history.first?.date, hovered: $hovered)
                .frame(height: 240)

            StatsGrid(item: item, series: series, currency: currency)
        }

        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                // The card image shrinks, then moves out of the way, as the window narrows.
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 36) {
                        info.frame(minWidth: 440, maxWidth: .infinity, alignment: .leading)
                        artwork(width: Self.artworkWidth)
                    }
                    HStack(alignment: .top, spacing: 24) {
                        info.frame(minWidth: 340, maxWidth: .infinity, alignment: .leading)
                        artwork(width: 160)
                    }
                    info
                }

                footer
            }
            .padding(.horizontal, 32)
            .padding(.vertical, 28)
            .frame(maxWidth: 1040, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .navigationTitle(item.card.name)
        .navigationSubtitle(item.subtitle)
    }

    /// The card image. Click it to see it larger.
    private func artwork(width: CGFloat) -> some View {
        Button {
            showingLargeImage = true
        } label: {
            CardArtwork(url: item.card.imageURL, width: width)
                .shadow(color: .black.opacity(0.18), radius: 14, y: 6)
        }
        .buttonStyle(.plain)
        .help("Click to enlarge")
        .accessibilityLabel("Card image")
        .accessibilityHint("Shows the card larger")
        .popover(isPresented: $showingLargeImage, arrowEdge: .leading) {
            CardArtwork(url: item.card.imageURL, width: 420)
                .padding(14)
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(item.card.name)
                .font(.largeTitle.weight(.semibold))
                .lineLimit(2)
                .textSelection(.enabled)
            Text(detailLine)
                .font(.title3)
                .foregroundStyle(.secondary)
        }
    }

    /// "Dominaria United · #107 · Foil · Mythic"
    private var detailLine: String {
        [item.card.setName, "#\(item.card.number)", item.finish.name, item.card.rarity]
            .compactMap { $0 }
            .filter { !$0.isEmpty }
            .joined(separator: " · ")
    }

    /// The price and the change over the chart's range. While hovering over
    /// the chart it shows that day's price instead, like Stocks.
    private func priceHeader(series: PriceSeries, currency: Currency) -> some View {
        let shownAmount = hovered?.amount ?? item.currentAmount(in: currency)
        let change: PriceChange? = {
            if let hovered, let baseline = series.baseline { return PriceChange(from: baseline.amount, to: hovered.amount) }
            return series.change
        }()

        return VStack(alignment: .leading, spacing: 4) {
            Text(PriceFormat.price(shownAmount, currency))
                .font(.system(size: 34, weight: .semibold))
                .monospacedDigit()
                .contentTransition(.numericText())
            HStack(spacing: 6) {
                if let change {
                    Text(changeText(change, currency: currency))
                        .foregroundStyle(change.direction.color)
                        .monospacedDigit()
                }
                Text(changeCaption(series: series, change: change))
                    .foregroundStyle(.secondary)
            }
            .font(.headline)
        }
        .accessibilityElement(children: .combine)
    }

    private func changeText(_ change: PriceChange, currency: Currency) -> String {
        let amount = PriceFormat.change(change.amount, currency)
        guard let fraction = change.fraction else { return amount }
        return "\(amount) (\(PriceFormat.percent(fraction)))"
    }

    private func changeCaption(series: PriceSeries, change: PriceChange?) -> String {
        if let hovered {
            return hovered.date.formatted(date: .abbreviated, time: .omitted)
        }
        if change == nil {
            return item.history.count <= 1 ? "History starts \(item.added.formatted(date: .abbreviated, time: .omitted))" : "No change"
        }
        return model.chartRange == .all ? "Since added" : "Past \(model.chartRange.name)"
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let error = item.lastError {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .symbolRenderingMode(.multicolor)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 10) {
                ForEach(item.card.links, id: \.url) { link in
                    Link(destination: link.url) {
                        Label("Buy on \(link.title)", systemImage: "arrow.up.right")
                            .labelStyle(TrailingIconLabelStyle())
                    }
                    .buttonStyle(.bordered)
                }
                if let page = item.card.pageURL {
                    Link(destination: page) {
                        Label(item.card.game.source.name, systemImage: "arrow.up.right")
                            .labelStyle(TrailingIconLabelStyle())
                    }
                    .buttonStyle(.bordered)
                }
            }
            .controlSize(.large)

            Text(attribution)
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    /// "Prices from Scryfall · TCGplayer and Cardmarket · Updated 2 hours ago"
    private var attribution: String {
        var parts = ["Prices from \(item.card.game.source.name)", "TCGplayer and Cardmarket"]
        if let updated = item.sourceUpdated ?? item.lastUpdated {
            parts.append("Updated \(updated.formatted(.relative(presentation: .named)))")
        }
        return parts.joined(separator: " · ")
    }
}

private struct TrailingIconLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 4) {
            configuration.title
            configuration.icon.imageScale(.small)
        }
    }
}

/// Label and value pairs in two columns, with hairlines between rows, like Stocks.
struct StatsGrid: View {
    var item: WatchItem
    var series: PriceSeries
    var currency: Currency

    private struct Stat: Identifiable {
        var label: String
        var value: String
        var id: String { label }
    }

    var body: some View {
        let rows = stride(from: 0, to: stats.count, by: 2).map { Array(stats[$0..<min($0 + 2, stats.count)]) }

        Grid(alignment: .leading, horizontalSpacing: 28, verticalSpacing: 0) {
            ForEach(rows.indices, id: \.self) { index in
                GridRow {
                    ForEach(rows[index]) { stat in
                        HStack {
                            Text(stat.label).foregroundStyle(.secondary)
                            Spacer(minLength: 12)
                            Text(stat.value).monospacedDigit()
                        }
                        .padding(.vertical, 7)
                    }
                    if rows[index].count == 1 { Color.clear.gridCellUnsizedAxes([.horizontal, .vertical]) }
                }
                if index < rows.count - 1 {
                    Divider()
                }
            }
        }
        .font(.callout)
    }

    private var stats: [Stat] {
        let price = item.price
        var stats: [Stat] = [
            Stat(label: "Low", value: PriceFormat.price(series.low, currency)),
            Stat(label: "High", value: PriceFormat.price(series.high, currency)),
            Stat(label: "Added", value: PriceFormat.price(item.addedAmount(in: currency), currency)),
            Stat(label: "Since Added", value: PriceFormat.change(item.changeSinceAdded(in: currency), currency)),
            Stat(label: "TCGplayer", value: PriceFormat.price(price?.usd, .usd)),
            Stat(label: "Cardmarket", value: PriceFormat.price(price?.eur, .eur)),
        ]
        if let low = price?.usdLow, let high = price?.usdHigh {
            stats.append(Stat(label: "TCGplayer Listings",
                              value: "\(PriceFormat.price(low, .usd)) – \(PriceFormat.price(high, .usd))"))
        }
        if let average = price?.eurAverage7 {
            stats.append(Stat(label: "Cardmarket 7-Day Avg.", value: PriceFormat.price(average, .eur)))
        }
        if let average = price?.eurAverage30 {
            stats.append(Stat(label: "Cardmarket 30-Day Avg.", value: PriceFormat.price(average, .eur)))
        }
        return stats
    }
}
