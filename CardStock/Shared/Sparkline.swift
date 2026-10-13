import CardStockKit
import SwiftUI
import WidgetKit

/// A small line of recent prices, like the ones in Stocks. The dotted line is
/// where the range started, so you can see at a glance if it's above or below.
struct Sparkline: View {
    var values: [Double]
    var direction: PriceChange.Direction
    var lineWidth: CGFloat = 1.5

    @Environment(\.widgetRenderingMode) private var renderingMode

    var body: some View {
        let lineColor: Color = renderingMode == .fullColor ? direction.color : .primary

        Canvas { context, size in
            guard let first = values.first else { return }
            let dash = StrokeStyle(lineWidth: 1, dash: [1.5, 2.5])

            // One price so far: a flat dotted line with a dot at today.
            guard values.count > 1, let low = values.min(), let high = values.max() else {
                var baseline = Path()
                baseline.move(to: CGPoint(x: 0, y: size.height / 2))
                baseline.addLine(to: CGPoint(x: size.width, y: size.height / 2))
                context.stroke(baseline, with: .color(.secondary.opacity(0.6)), style: dash)
                let dot = CGRect(x: size.width - 4, y: size.height / 2 - 2, width: 4, height: 4)
                context.fill(Path(ellipseIn: dot), with: .color(.secondary))
                return
            }

            let inset = lineWidth
            let span = high - low
            func point(_ index: Int, _ value: Double) -> CGPoint {
                let x = size.width * CGFloat(index) / CGFloat(values.count - 1)
                let usable = size.height - inset * 2
                let y = span == 0 ? size.height / 2 : inset + usable * (1 - CGFloat((value - low) / span))
                return CGPoint(x: x, y: y)
            }

            let baselineY = point(0, first).y
            var baseline = Path()
            baseline.move(to: CGPoint(x: 0, y: baselineY))
            baseline.addLine(to: CGPoint(x: size.width, y: baselineY))
            context.stroke(baseline, with: .color(.secondary.opacity(0.5)), style: dash)

            var line = Path()
            for (index, value) in values.enumerated() {
                if index == 0 {
                    line.move(to: point(index, value))
                } else {
                    line.addLine(to: point(index, value))
                }
            }
            context.stroke(line, with: .color(lineColor),
                           style: StrokeStyle(lineWidth: lineWidth, lineCap: .round, lineJoin: .round))
        }
        .accessibilityHidden(true)
    }
}

extension PriceChange.Direction {
    /// System green and red, like Stocks. Flat is gray.
    var color: Color {
        switch self {
        case .up: .green
        case .down: .red
        case .flat: .secondary
        }
    }
}

extension Optional where Wrapped == PriceChange {
    var direction: PriceChange.Direction { self?.direction ?? .flat }
}
