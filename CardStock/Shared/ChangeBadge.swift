import CardStockKit
import SwiftUI

/// The green or red pill with the change in it.
struct ChangeBadge: View {
    var change: PriceChange?
    var currency: Currency
    var asPercent: Bool = true
    var minWidth: CGFloat = 66
    var font: Font = .callout.weight(.medium)

    var body: some View {
        Text(PriceFormat.change(change, currency, asPercent: asPercent))
            .font(font)
            .monospacedDigit()
            .lineLimit(1)
            .foregroundStyle(change == nil || change.direction == .flat ? Color.primary : Color.white)
            .padding(.horizontal, 6)
            .frame(minWidth: minWidth, minHeight: 22, alignment: .trailing)
            .background(background, in: RoundedRectangle(cornerRadius: 5, style: .continuous))
    }

    private var background: Color {
        switch change.direction {
        case .up: .green
        case .down: .red
        case .flat: Color.secondary.opacity(0.2)
        }
    }
}
