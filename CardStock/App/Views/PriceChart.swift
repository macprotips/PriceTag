import Charts
import CardStockKit
import SwiftUI

/// The price line for a range. One color for the whole line (green if the
/// range is up, red if it's down), a faint fill under it, and a dotted line
/// at the starting price. Hovering shows a rule and that day's price.
struct PriceChart: View {
    var series: PriceSeries
    var currency: Currency
    @Binding var hovered: PriceSeries.Point?

    @State private var hoveredDate: Date?

    var body: some View {
        if series.points.count < 2 {
            emptyState
        } else {
            chart
        }
    }

    private var color: Color { series.change.direction.color }

    private var chart: some View {
        let domain = yDomain

        return Chart {
            if let baseline = series.baseline {
                RuleMark(y: .value("Start", baseline.value))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [2, 3]))
                    .foregroundStyle(Color.secondary.opacity(0.6))
            }

            ForEach(series.points) { point in
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
            Text("History starts today")
                .font(.headline)
            Text("CardStock records this card's price once a day. Check back tomorrow to see it move.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 320)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(.quinary))
    }

    // MARK: Scales

    /// The lowest and highest price with some room around them, so the line
    /// never touches the edges. A flat line sits in the middle.
    private var yDomain: ClosedRange<Double> {
        var values = series.points.map(\.value)
        if let baseline = series.baseline { values.append(baseline.value) }
        let low = values.min() ?? 0
        let high = values.max() ?? 1
        let padding = high > low ? (high - low) * 0.12 : max(high * 0.05, 0.05)
        return max(0, low - padding)...(high + padding)
    }

    private var xDomain: ClosedRange<Date> {
        let first = series.start ?? series.points.first?.date ?? Date()
        let last = series.points.last?.date ?? Date()
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
