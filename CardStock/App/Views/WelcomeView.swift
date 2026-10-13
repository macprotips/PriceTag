import AppKit
import CardStockKit
import SwiftUI

/// The first thing people see, before they've added a card.
struct WelcomeView: View {
    @Environment(AppModel.self) private var model

    private struct Suggestion: Identifiable {
        var game: Game
        var name: String
        var id: String { name }
    }

    private let suggestions = [
        Suggestion(game: .pokemon, name: "Charizard"),
        Suggestion(game: .magic, name: "Sheoldred"),
        Suggestion(game: .pokemon, name: "Umbreon"),
        Suggestion(game: .magic, name: "The One Ring"),
    ]

    var body: some View {
        ScrollView {
            VStack(spacing: 32) {
                VStack(spacing: 14) {
                    Image(nsImage: NSApp.applicationIconImage)
                        .resizable()
                        .frame(width: 96, height: 96)
                        .accessibilityHidden(true)
                    Text("Welcome to CardStock")
                        .font(.system(size: 30, weight: .bold))
                    Text("Follow Magic and Pokémon card prices\nthe way Stocks follows shares.")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }

                VStack(alignment: .leading, spacing: 20) {
                    FeatureRow(symbol: "chart.xyaxis.line", color: .green,
                               title: "Prices over time",
                               text: "Daily prices from TCGplayer and Cardmarket, charted from the day you add a card.")
                    FeatureRow(symbol: "dollarsign.circle", color: .blue,
                               title: "Your collection’s value",
                               text: "Enter how many you own and what you paid to see what they’re worth.")
                    FeatureRow(symbol: "bell.badge", color: .orange,
                               title: "Price alerts",
                               text: "Get a notification when a card rises above or falls below a price you choose.")
                    FeatureRow(symbol: "rectangle.3.group", color: .purple,
                               title: "Desktop widgets",
                               text: "Keep a card, your watchlist or your collection’s value on your desktop.")
                }
                .frame(maxWidth: 420)

                VStack(spacing: 14) {
                    Button {
                        model.isAddingCard = true
                    } label: {
                        Text("Add Your First Card")
                            .frame(minWidth: 220)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.extraLarge)
                    .keyboardShortcut(.defaultAction)

                    HStack(spacing: 8) {
                        Text("Or try")
                            .foregroundStyle(.secondary)
                        ForEach(suggestions) { suggestion in
                            Button(suggestion.name) {
                                model.startAdding(game: suggestion.game, query: suggestion.name)
                            }
                            .buttonStyle(.bordered)
                            .buttonBorderShape(.capsule)
                            .controlSize(.small)
                        }
                    }
                    .font(.callout)
                }
            }
            .padding(48)
            .frame(maxWidth: .infinity, minHeight: 560)
        }
    }
}

private struct FeatureRow: View {
    var symbol: String
    var color: Color
    var title: String
    var text: String

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: symbol)
                .font(.system(size: 24, weight: .regular))
                .foregroundStyle(color)
                .frame(width: 36)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(.headline)
                Text(text)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .accessibilityElement(children: .combine)
    }
}
