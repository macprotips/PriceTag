import CardStockKit
import Charts
import SwiftUI

/// The price line for a range, like Stocks: one color for the whole line
/// (green if the range is up, red if down), a faint fill under it, and a
/// dotted line at the starting price. Hovering shows a rule and that day's
/// price, which the header above picks up.
struct PriceChart: View {
    var series: PriceSeries
    var currency: Currency
    var range: ChartRange
    /// When the card's history began, for the note under a new card's chart.
    var historyStart: Date?
    @Binding var hovered: PriceSeries.Point?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            chart
            if plotted.count < 2 {
                Text(note)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
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
        let points = plotted

        return Chart {
            if let baseline = series.baseline {
                RuleMark(y: .value("Start", baseline.value))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [2, 3]))
                    .foregroundStyle(Color.secondary.opacity(0.55))
            }

            if points.count > 1 {
                ForEach(points) { point in
                    AreaMark(
                        x: .value("Date", point.date),
                        yStart: .value("Floor", domain.lowerBound),
                        yEnd: .value("Price", point.value)
                    )
                    .foregroundStyle(
                        LinearGradient(colors: [color.opacity(0.18), color.opacity(0)], startPoint: .top, endPoint: .bottom)
                    )

                    LineMark(x: .value("Date", point.date), y: .value("Price", point.value))
                        .foregroundStyle(color)
                        .lineStyle(StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                }
            } else if let only = points.first {
                // One price so far: mark it at the right edge.
                PointMark(x: .value("Date", only.date), y: .value("Price", only.value))
                    .foregroundStyle(Color.secondary)
                    .symbolSize(36)
            }

            if let hovered {
                RuleMark(x: .value("Date", hovered.date))
                    .foregroundStyle(Color.secondary.opacity(0.45))
                    .lineStyle(StrokeStyle(lineWidth: 1))
                PointMark(x: .value("Date", hovered.date), y: .value("Price", hovered.value))
                    .symbol {
                        Circle()
                            .fill(color)
                            .frame(width: 10, height: 10)
                            .overlay(Circle().stroke(Color(nsColor: .windowBackgroundColor), lineWidth: 2))
                    }
            }
        }
        .chartYScale(domain: domain)
        .chartXScale(domain: xDomain)
        .chartYAxis {
            AxisMarks(position: .trailing, values: .automatic(desiredCount: 4)) { value in
                AxisGridLine().foregroundStyle(Color.secondary.opacity(0.15))
                AxisValueLabel {
                    if let amount = value.as(Double.self) {
                        Text(axisPrice(amount))
                            .monospacedDigit()
                    }
                }
            }
        }
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: range == .week ? 7 : 5)) { _ in
                AxisGridLine().foregroundStyle(Color.secondary.opacity(0.08))
                AxisValueLabel(format: xFormat)
            }
        }
        .chartOverlay { proxy in
            // Follow the pointer, like Stocks, rather than needing a drag.
            GeometryReader { geometry in
                Rectangle()
                    .fill(.clear)
                    .contentShape(Rectangle())
                    .onContinuousHover { phase in
                        switch phase {
                        case .active(let location):
                            let plotX = proxy.plotFrame.map { geometry[$0].origin.x } ?? 0
                            let date: Date? = proxy.value(atX: location.x - plotX)
                            hovered = series.points.count > 1 ? date.flatMap(nearestPoint(to:)) : nil
                        case .ended:
                            hovered = nil
                        }
                    }
            }
        }
        .accessibilityLabel(accessibilitySummary)
    }

    // MARK: Axes

    private var xFormat: Date.FormatStyle {
        switch range {
        case .week: .dateTime.weekday(.abbreviated)
        case .month, .threeMonths: .dateTime.month(.abbreviated).day()
        case .sixMonths, .year, .all: .dateTime.month(.abbreviated).year(.twoDigits)
        }
    }

    /// Round numbers on the axis: "$80" rather than "$80.00", unless the
    /// prices are small enough that cents matter.
    private func axisPrice(_ value: Double) -> String {
        let span = yDomain.upperBound - yDomain.lowerBound
        let decimals = span >= 5 ? 0 : 2
        return Decimal(value).formatted(.currency(code: currency.code).precision(.fractionLength(decimals)))
    }

    /// The lowest and highest price with some room around them, so the line
    /// never touches the edges. A flat line sits in the middle.
    private var yDomain: ClosedRange<Double> {
        var values = plotted.map(\.value)
        if let baseline = series.baseline { values.append(baseline.value) }
        let low = values.min() ?? 0
        let high = values.max() ?? 1
        let padding = high > low ? (high - low) * 0.15 : max(high * 0.1, 0.1)
        return max(0, low - padding)...(high + padding)
    }

    /// From the range's start to now. A card with one price shows it at the
    /// right edge of the range rather than alone in the middle.
    private var xDomain: ClosedRange<Date> {
        let now = Date()
        let last = max(plotted.last?.date ?? now, now)
        if plotted.count < 2 {
            let start = range.start(endingAt: last) ?? last.addingTimeInterval(-7 * 24 * 60 * 60)
            return start...last
        }
        let first = plotted.first?.date ?? last
        return first...max(last, first.addingTimeInterval(60 * 60))
    }

    private func nearestPoint(to date: Date) -> PriceSeries.Point? {
        series.points.min { abs($0.date.timeIntervalSince(date)) < abs($1.date.timeIntervalSince(date)) }
    }

    private var note: String {
        if let historyStart, !Calendar.current.isDateInToday(historyStart) {
            return "Not enough history in this range yet. Try a longer range."
        }
        return "Price history starts today. CardStock saves the price once a day, so the chart fills in over time."
    }

    private var accessibilitySummary: String {
        guard let first = series.points.first, let last = series.points.last, series.points.count > 1 else {
            return "Price chart. History starts today."
        }
        let low = PriceFormat.price(series.low, currency)
        let high = PriceFormat.price(series.high, currency)
        return "Price chart from \(first.date.formatted(date: .abbreviated, time: .omitted)) to "
            + "\(last.date.formatted(date: .abbreviated, time: .omitted)), "
            + "\(PriceFormat.spokenChange(series.change, currency)), low \(low), high \(high)"
    }
}
