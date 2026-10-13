import CardStockKit
import SwiftUI

/// How many copies you own and what you paid for each, with their value.
struct HoldingEditor: View {
    @Environment(AppModel.self) private var model
    var item: WatchItem

    @State private var quantity = 0
    @State private var pricePaid: Decimal?

    /// The currency the price paid is in: what it was entered in, or the
    /// current one for a new entry.
    private var currency: Currency { item.holding?.currency ?? model.currency }

    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
            GridRow {
                Text("Copies owned").foregroundStyle(.secondary)
                HStack(spacing: 6) {
                    TextField("Copies owned", value: $quantity, format: .number)
                        .labelsHidden()
                        .multilineTextAlignment(.trailing)
                        .frame(width: 56)
                    Stepper("Copies owned", value: $quantity, in: 0...9999)
                        .labelsHidden()
                }
            }
            GridRow {
                Text("Price paid each").foregroundStyle(.secondary)
                TextField("Price paid each", value: $pricePaid, format: .currency(code: currency.code), prompt: Text("Optional"))
                    .labelsHidden()
                    .multilineTextAlignment(.trailing)
                    .frame(width: 112)
                    .disabled(quantity == 0)
            }
            if quantity > 0 {
                Divider().gridCellUnsizedAxes(.horizontal)
                GridRow {
                    Text("Value").foregroundStyle(.secondary)
                    Text(PriceFormat.price(value, model.currency))
                        .fontWeight(.semibold)
                        .monospacedDigit()
                }
                if let gain {
                    GridRow {
                        Text("Gain").foregroundStyle(.secondary)
                        Text(gainText(gain))
                            .fontWeight(.semibold)
                            .monospacedDigit()
                            .foregroundStyle(gain.direction.color)
                    }
                }
            }
        }
        .font(.callout)
        .onAppear(perform: load)
        .onChange(of: item.id) { load() }
        .onChange(of: quantity) { save() }
        .onChange(of: pricePaid) { save() }
    }

    private var value: Amount? {
        item.currentAmount(in: model.currency).map { Amount(cents: $0.cents * quantity) }
    }

    /// Only when the price paid is in the currency being shown.
    private var gain: PriceChange? {
        guard let pricePaid, currency == model.currency,
              let current = item.currentAmount(in: currency)
        else { return nil }
        let paid = Amount(decimal: pricePaid)
        return PriceChange(from: Amount(cents: paid.cents * quantity), to: Amount(cents: current.cents * quantity))
    }

    private func gainText(_ change: PriceChange) -> String {
        let amount = PriceFormat.change(change.amount, model.currency)
        guard let fraction = change.fraction else { return amount }
        return "\(amount) (\(PriceFormat.percent(fraction)))"
    }

    private func load() {
        quantity = item.holding?.quantity ?? 0
        pricePaid = item.holding?.pricePaid?.decimal
    }

    private func save() {
        let clamped = max(0, min(quantity, 9999))
        if clamped != quantity { quantity = clamped; return }
        let paid = pricePaid.map(Amount.init(decimal:)).flatMap { $0.cents >= 0 ? $0 : nil }
        model.setHolding(quantity > 0 ? Holding(quantity: quantity, pricePaid: paid, currency: currency) : nil, for: item.id)
    }
}

/// Price levels to get a notification at.
struct AlertEditor: View {
    @Environment(AppModel.self) private var model
    var item: WatchItem

    @State private var above: Decimal?
    @State private var below: Decimal?

    private var currency: Currency { item.alert?.currency ?? model.currency }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                GridRow {
                    Text("Rises above").foregroundStyle(.secondary)
                    field("Rises above", value: $above)
                }
                GridRow {
                    Text("Falls below").foregroundStyle(.secondary)
                    field("Falls below", value: $below)
                }
            }
            Text("You’ll get a notification when the price crosses either level.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.callout)
        .onAppear(perform: load)
        .onChange(of: item.id) { load() }
        .onChange(of: above) { save() }
        .onChange(of: below) { save() }
    }

    private func field(_ title: String, value: Binding<Decimal?>) -> some View {
        HStack(spacing: 4) {
            TextField(title, value: value, format: .currency(code: currency.code), prompt: Text("None"))
                .labelsHidden()
                .multilineTextAlignment(.trailing)
                .frame(width: 112)
            Button {
                value.wrappedValue = nil
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(.tertiary)
            }
            .buttonStyle(.plain)
            .help("Clear")
            .accessibilityLabel("Clear \(title.lowercased())")
            .opacity(value.wrappedValue == nil ? 0 : 1)
            .disabled(value.wrappedValue == nil)
        }
    }

    private func load() {
        above = item.alert?.above?.decimal
        below = item.alert?.below?.decimal
    }

    private func save() {
        let level = { (value: Decimal?) in value.map(Amount.init(decimal:)).flatMap { $0.isPositive ? $0 : nil } }
        model.setAlert(PriceAlert(above: level(above), below: level(below), currency: currency), for: item.id)
    }
}
