import CardStockKit
import SwiftUI

/// A small line of recent prices, like the ones in Stocks. The dotted line is
/// where the range started, so you can see at a glance if it's above or below.
struct Sparkline: View {
    var values: [Double]
    var direction: PriceChange.Direction

    var body: some View {
        Canvas { context, size in
            guard values.count > 1, let low = values.min(), let high = values.max() else { return }
            let span = high - low
            func point(_ index: Int, _ value: Double) -> CGPoint {
                let x = size.width * CGFloat(index) / CGFloat(values.count - 1)
                let y = span == 0 ? size.height / 2 : size.height * (1 - CGFloat((value - low) / span))
                return CGPoint(x: x, y: y)
            }

            let baselineY = point(0, values[0]).y
            var baseline = Path()
            baseline.move(to: CGPoint(x: 0, y: baselineY))
            baseline.addLine(to: CGPoint(x: size.width, y: baselineY))
            context.stroke(baseline, with: .color(.secondary.opacity(0.5)),
                           style: StrokeStyle(lineWidth: 1, dash: [1.5, 2.5]))

            var line = Path()
            for (index, value) in values.enumerated() {
                if index == 0 {
                    line.move(to: point(index, value))
                } else {
                    line.addLine(to: point(index, value))
                }
            }
            context.stroke(line, with: .color(direction.color),
                           style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
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
