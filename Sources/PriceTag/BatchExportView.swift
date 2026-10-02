import PriceTagKit
import SwiftUI

/// Paste a list of prices (one per line) and export them all in one go.
struct BatchExportView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""

    private var prices: [FormattedPrice] { model.batchPrices(from: text) }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Batch Export")
                .font(.title2.weight(.bold))
            Text("Paste or type one price per line. Each one is saved as its own PNG in your export folder, with the current look.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            HStack(alignment: .top, spacing: 14) {
                TextEditor(text: $text)
                    .font(.system(size: 15, design: .monospaced))
                    .scrollContentBackground(.hidden)
                    .padding(8)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: .textBackgroundColor)))
                    .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.primary.opacity(0.12)))

                List(prices, id: \.text) { price in
                    HStack {
                        Circle()
                            .fill(Color(hex: model.style.color(for: price).palette.bottom))
                            .frame(width: 10, height: 10)
                        Text(price.text).font(.system(.body, design: .rounded).weight(.semibold))
                    }
                }
                .frame(width: 180)
                .overlay {
                    if prices.isEmpty {
                        Text("Prices show up here")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .frame(height: 260)

            HStack {
                Label(model.exportFolder.lastPathComponent, systemImage: "folder")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                Button("Change…") { model.chooseExportFolder() }
                    .controlSize(.small)
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Export \(prices.count) PNG\(prices.count == 1 ? "" : "s")") {
                    let written = model.exportBatch(prices)
                    if !written.isEmpty { dismiss() }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(prices.isEmpty)
            }
        }
        .padding(22)
        .frame(width: 560)
    }
}
