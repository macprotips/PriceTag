import Charts
import CardStockKit
import SwiftUI

/// The price line for a range. One color for the whole line (green if the
/// range is up, red if it's down), a faint fill under it, and a dotted line
/// at the starting price. Hovering shows a rule and that day's price.
struct PriceChart: View {
    var series: PriceSeries
    var currency: Currency
    /// When the card's history began, for the empty state.
    var historyStart: Date?
    @Binding var hovered: PriceSeries.Point?

    @State private var hoveredDate: Date?

    var body: some View {
        if plotted.count < 2 {
            emptyState
        } else {
            chart
        }
    }

    private var color: Color { series.change.direction.color }

    /// The points to draw. When the price before the range is known, the line
    /// starts at the left edge with that price, since it held until the next
    /// one, rather than leaving a gap.
    private var plotted: [PriceSeries.Point] {
        guard let baseline = series.baseline, let start = series.start, baseline.date < start else {
            return series.points
        }
        return [PriceSeries.Point(date: start, amount: baseline.amount)] + series.points
    }

    private var chart: some View {
        let domain = yDomain

        return Chart {
            if let baseline = series.baseline {
                RuleMark(y: .value("Start", baseline.value))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [2, 3]))
                    .foregroundStyle(Color.secondary.opacity(0.6))
            }

            ForEach(plotted) { point in
                AreaMark(
                    x: .value("Date", point.date),
                    yStart: .value("Floor", domain.lowerBound),
                    yEnd: .value("Price", point.value)
                )
                .foregroundStyle(
                    LinearGradient(colors: [color.opacity(0.16), color.opacity(0)], startPoint: .top, endPoint: .bottom)
                )

                LineMark(x: .value("Date", point.date), y: .value("Price", point.value))
                    .foregroundStyle(color)
                    .lineStyle(StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
            }

            if let hovered {
                RuleMark(x: .value("Date", hovered.date))
                    .foregroundStyle(Color.secondary.opacity(0.5))
                PointMark(x: .value("Date", hovered.date), y: .value("Price", hovered.value))
                    .foregroundStyle(color)
                    .symbolSize(50)
            }
        }
        .chartYScale(domain: domain)
        .chartXScale(domain: xDomain)
        .chartXSelection(value: $hoveredDate)
        .chartYAxis {
            AxisMarks(position: .trailing, values: .automatic(desiredCount: 4)) { value in
                AxisGridLine().foregroundStyle(Color.secondary.opacity(0.15))
                AxisValueLabel {
                    if let amount = value.as(Double.self) {
                        Text(PriceFormat.price(Amount(double: amount), currency))
                    }
                }
            }
        }
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: 5)) { _ in
                AxisValueLabel(format: .dateTime.month(.abbreviated).day())
            }
        }
        .onChange(of: hoveredDate) { _, date in
            hovered = date.flatMap(nearestPoint(to:))
        }
        .accessibilityLabel(accessibilitySummary)
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: "chart.xyaxis.line")
                .font(.system(size: 28, weight: .light))
                .foregroundStyle(.tertiary)
            Text(emptyTitle)
                .font(.headline)
            Text(emptyMessage)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 320)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(.quinary))
    }

    private var startsToday: Bool {
        guard let historyStart else { return true }
        return Calendar.current.isDateInToday(historyStart)
    }

    private var emptyTitle: String {
        startsToday ? "History starts today" : "Not enough history yet"
    }

    private var emptyMessage: String {
        if startsToday {
            return "CardStock saves this card's price once a day. Check back tomorrow to see it move."
        }
        return "There aren't two prices in this range yet. Try a longer range, or check back tomorrow."
    }

    // MARK: Scales

    /// The lowest and highest price with some room around them, so the line
    /// never touches the edges. A flat line sits in the middle.
    private var yDomain: ClosedRange<Double> {
        var values = plotted.map(\.value)
        if let baseline = series.baseline { values.append(baseline.value) }
        let low = values.min() ?? 0
        let high = values.max() ?? 1
        let padding = high > low ? (high - low) * 0.12 : max(high * 0.05, 0.05)
        return max(0, low - padding)...(high + padding)
    }

    private var xDomain: ClosedRange<Date> {
        let first = plotted.first?.date ?? Date()
        let last = plotted.last?.date ?? Date()
        return first...max(last, first.addingTimeInterval(60 * 60))
    }

    private func nearestPoint(to date: Date) -> PriceSeries.Point? {
        series.points.min { abs($0.date.timeIntervalSince(date)) < abs($1.date.timeIntervalSince(date)) }
    }

    private var accessibilitySummary: String {
        guard let first = series.points.first, let last = series.points.last else { return "No price history" }
        let low = PriceFormat.price(series.low, currency)
        let high = PriceFormat.price(series.high, currency)
        return "Price chart from \(first.date.formatted(date: .abbreviated, time: .omitted)) to "
            + "\(last.date.formatted(date: .abbreviated, time: .omitted)), "
            + "\(PriceFormat.spokenChange(series.change, currency)), low \(low), high \(high)"
    }
}
