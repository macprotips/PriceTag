import CardStockKit
import SwiftUI
import WidgetKit

/// The green or red pill with the change in it, like Stocks.
///
/// On the desktop, macOS tints widgets to a single color when another app is
/// in front. A filled pill would lose its text there, so it becomes an
/// outline and the sign carries the direction.
struct ChangeBadge: View {
    var change: PriceChange?
    var currency: Currency
    var asPercent: Bool = true
    var minWidth: CGFloat = 68
    var font: Font = .callout.weight(.semibold)

    @Environment(\.widgetRenderingMode) private var renderingMode

    var body: some View {
        let direction = change.direction
        let filled = renderingMode == .fullColor && direction != .flat && change != nil

        Text(PriceFormat.change(change, currency, asPercent: asPercent))
            .font(font)
            .monospacedDigit()
            .lineLimit(1)
            .minimumScaleFactor(0.8)
            .foregroundStyle(filled ? AnyShapeStyle(Color.white) : AnyShapeStyle(HierarchicalShapeStyle.primary))
            .padding(.horizontal, 7)
            .frame(minWidth: minWidth, minHeight: 22, alignment: .trailing)
            .background {
                let shape = RoundedRectangle(cornerRadius: 6, style: .continuous)
                if filled {
                    shape.fill(direction.color.gradient)
                } else if renderingMode == .fullColor {
                    shape.fill(.quaternary)
                } else {
                    shape.strokeBorder(.secondary, lineWidth: 1)
                }
            }
            .contentTransition(.numericText())
    }
}
