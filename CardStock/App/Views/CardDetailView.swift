import AppKit
import CardStockKit
import SwiftUI

struct CardDetailView: View {
    @Environment(AppModel.self) private var model
    var item: WatchItem

    /// The point under the pointer in the chart, if any.
    @State private var hovered: PriceSeries.Point?
    @State private var showingLargeImage = false
    @ScaledMetric(relativeTo: .largeTitle) private var priceSize: CGFloat = 34

    private static let artworkWidth: CGFloat = 260

    var body: some View {
        @Bindable var model = model
        let currency = model.currency
        let series = item.series(in: currency, range: model.chartRange)

        let info = VStack(alignment: .leading, spacing: 20) {
            header
            priceHeader(series: series, currency: currency)

            Picker("Range", selection: $model.chartRange) {
                ForEach(ChartRange.allCases) { range in
                    Text(range.title).tag(range)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 340)

            PriceChart(series: series, currency: currency, range: model.chartRange,
                       historyStart: item.history.first?.date, hovered: $hovered)
                .frame(height: 250)
        }

        ScrollView {
            VStack(alignment: .leading, spacing: 32) {
                // The card image shrinks, then moves out of the way, as the window narrows.
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 40) {
                        info.frame(minWidth: 440, maxWidth: .infinity, alignment: .leading)
                        artwork(width: Self.artworkWidth)
                    }
                    HStack(alignment: .top, spacing: 28) {
                        info.frame(minWidth: 340, maxWidth: .infinity, alignment: .leading)
                        artwork(width: 160)
                    }
                    info
                }

                DetailSection("Statistics") {
                    StatsGrid(item: item, series: series, currency: currency)
                }

                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 40) {
                        holdingSection.frame(maxWidth: .infinity, alignment: .topLeading)
                        alertSection.frame(maxWidth: .infinity, alignment: .topLeading)
                    }
                    VStack(alignment: .leading, spacing: 32) {
                        holdingSection
                        alertSection
                    }
                }

                footer
            }
            .padding(.horizontal, 36)
            .padding(.top, 28)
            .padding(.bottom, 36)
            .frame(maxWidth: 1060, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
    }

    // MARK: Header

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(item.card.name)
                .font(.largeTitle.weight(.bold))
                .lineLimit(2)
                .textSelection(.enabled)
            Text(detailLine)
                .font(.title3)
                .foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
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
            if let hovered {
                guard let baseline = series.baseline, hovered.date != baseline.date else { return nil }
                return PriceChange(from: baseline.amount, to: hovered.amount)
            }
            return series.change
        }()

        return VStack(alignment: .leading, spacing: 4) {
            Text(PriceFormat.price(shownAmount, currency))
                .font(.system(size: priceSize, weight: .semibold))
                .monospacedDigit()
                .contentTransition(.numericText())
            HStack(spacing: 6) {
                if let change {
                    Text(changeText(change, currency: currency))
                        .foregroundStyle(change.direction.color)
                        .monospacedDigit()
                }
                Text(changeCaption(change: change))
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

    private func changeCaption(change: PriceChange?) -> String {
        if let hovered {
            return hovered.date.formatted(date: .abbreviated, time: .omitted)
        }
        if change == nil {
            if item.history.count <= 1 {
                return Calendar.current.isDateInToday(item.added)
                    ? "Added today"
                    : "Added \(item.added.formatted(date: .abbreviated, time: .omitted))"
            }
            return "No change"
        }
        switch model.chartRange {
        case .week: return "Past week"
        case .month: return "Past month"
        case .threeMonths: return "Past 3 months"
        case .sixMonths: return "Past 6 months"
        case .year: return "Past year"
        case .all: return "Since added"
        }
    }

    // MARK: Artwork

    /// The card image. Click it to see it larger.
    private func artwork(width: CGFloat) -> some View {
        Button {
            showingLargeImage = true
        } label: {
            CardArtwork(url: item.card.imageURL, width: width)
                .shadow(color: .black.opacity(0.22), radius: 18, y: 8)
        }
        .buttonStyle(ArtworkButtonStyle())
        .help("Click to Enlarge")
        .accessibilityLabel("\(item.card.name) card image")
        .accessibilityHint("Shows the card larger")
        .popover(isPresented: $showingLargeImage) {
            CardArtwork(url: item.card.imageURL, width: 440)
                .padding(16)
        }
    }

    // MARK: Holding and alert

    private var holdingSection: some View {
        DetailSection("Your Copies") {
            HoldingEditor(item: item)
        }
    }

    private var alertSection: some View {
        DetailSection("Price Alert") {
            AlertEditor(item: item)
        }
    }

    // MARK: Footer

    private var footer: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let error = item.lastError {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .symbolRenderingMode(.multicolor)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 18) {
                ForEach(item.card.links, id: \.url) { link in
                    Link(destination: link.url) {
                        Label("Buy on \(link.title)", systemImage: "arrow.up.forward")
                            .labelStyle(TrailingIconLabelStyle())
                    }
                }
                if let page = item.card.pageURL {
                    Link(destination: page) {
                        Label("View on \(item.card.game.source.name)", systemImage: "arrow.up.forward")
                            .labelStyle(TrailingIconLabelStyle())
                    }
                }
            }
            .font(.callout.weight(.medium))

            Text(attribution)
                .font(.footnote)
                .foregroundStyle(.tertiary)
        }
    }

    /// "Prices from TCGplayer and Cardmarket via Scryfall · Updated 2 hours ago"
    private var attribution: String {
        var parts = ["Prices from TCGplayer and Cardmarket via \(item.card.game.source.name)"]
        if let updated = item.sourceUpdated ?? item.lastUpdated {
            parts.append("Updated \(updated.formatted(.relative(presentation: .named)))")
        }
        return parts.joined(separator: " · ")
    }
}

/// A titled block in the detail view.
struct DetailSection<Content: View>: View {
    var title: String
    var content: Content

    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(title)
                .font(.title3.weight(.semibold))
                .accessibilityAddTraits(.isHeader)
            content
        }
    }
}

private struct TrailingIconLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 3) {
            configuration.title
            configuration.icon.imageScale(.small)
        }
    }
}

/// Lifts the card slightly on hover and presses it on click.
private struct ArtworkButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        ArtworkButton(configuration: configuration)
    }

    private struct ArtworkButton: View {
        var configuration: ButtonStyleConfiguration
        @State private var isHovering = false
        @Environment(\.accessibilityReduceMotion) private var reduceMotion

        var body: some View {
            configuration.label
                .scaleEffect(reduceMotion ? 1 : configuration.isPressed ? 0.985 : isHovering ? 1.012 : 1)
                .animation(.snappy(duration: 0.2), value: isHovering)
                .animation(.snappy(duration: 0.12), value: configuration.isPressed)
                .onHover { isHovering = $0 }
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
        let stats = self.stats
        let rows = stride(from: 0, to: stats.count, by: 2).map { Array(stats[$0..<min($0 + 2, stats.count)]) }

        Grid(alignment: .leading, horizontalSpacing: 32, verticalSpacing: 0) {
            Divider()
            ForEach(rows.indices, id: \.self) { index in
                GridRow {
                    ForEach(rows[index]) { stat in
                        HStack(alignment: .firstTextBaseline) {
                            Text(stat.label).foregroundStyle(.secondary)
                            Spacer(minLength: 12)
                            Text(stat.value)
                                .monospacedDigit()
                                .textSelection(.enabled)
                        }
                        .padding(.vertical, 8)
                        .accessibilityElement(children: .combine)
                    }
                    if rows[index].count == 1 { Color.clear.gridCellUnsizedAxes([.horizontal, .vertical]) }
                }
                Divider()
            }
        }
        .font(.callout)
    }

    private var stats: [Stat] {
        let price = item.price
        var stats: [Stat] = []
        if series.points.count > 1 {
            stats.append(Stat(label: "Low", value: PriceFormat.price(series.low, currency)))
            stats.append(Stat(label: "High", value: PriceFormat.price(series.high, currency)))
        }
        stats.append(Stat(label: "Price when added", value: PriceFormat.price(item.addedAmount(in: currency), currency)))
        stats.append(Stat(label: "Since added", value: changeText(item.changeSinceAdded(in: currency))))
        stats.append(Stat(label: "TCGplayer market", value: PriceFormat.price(price?.usd, .usd)))
        stats.append(Stat(label: "Cardmarket trend", value: PriceFormat.price(price?.eur, .eur)))
        if let low = price?.usdLow, let high = price?.usdHigh {
            stats.append(Stat(label: "TCGplayer listings",
                              value: "\(PriceFormat.price(low, .usd))–\(PriceFormat.price(high, .usd))"))
        }
        if let average = price?.eurAverage7 {
            stats.append(Stat(label: "Cardmarket 7-day avg.", value: PriceFormat.price(average, .eur)))
        }
        if let average = price?.eurAverage30 {
            stats.append(Stat(label: "Cardmarket 30-day avg.", value: PriceFormat.price(average, .eur)))
        }
        return stats
    }

    private func changeText(_ change: PriceChange?) -> String {
        guard let change else { return "—" }
        let amount = PriceFormat.change(change.amount, currency)
        guard let fraction = change.fraction else { return amount }
        return "\(amount) (\(PriceFormat.percent(fraction)))"
    }
}
