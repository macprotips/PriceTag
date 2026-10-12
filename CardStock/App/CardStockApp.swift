import CardStockKit
import SwiftUI

@main
struct CardStockApp: App {
    @State private var model = AppModel()

    init() {
        // Card images are fetched with AsyncImage, which uses the shared cache.
        URLCache.shared = URLCache(memoryCapacity: 64 * 1024 * 1024, diskCapacity: 512 * 1024 * 1024)
    }

    var body: some Scene {
        Window("CardStock", id: "main") {
            ContentView()
                .environment(model)
                .frame(minWidth: 880, minHeight: 560)
        }
        .defaultSize(width: 1120, height: 720)
        .commands { CardStockCommands(model: model) }

        Settings {
            SettingsView()
                .environment(model)
        }
    }
}

struct CardStockCommands: Commands {
    let model: AppModel

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("Add Card…") { model.isAddingCard = true }
                .keyboardShortcut("n")
        }

        CommandGroup(before: .toolbar) {
            Button("Refresh Prices") { Task { await model.refresh() } }
                .keyboardShortcut("r")
                .disabled(model.isRefreshing || model.items.isEmpty)

            Divider()

            Menu("Chart Range") {
                ForEach(Array(ChartRange.allCases.enumerated()), id: \.element) { index, range in
                    Toggle(range.name.capitalized, isOn: Binding(
                        get: { model.chartRange == range },
                        set: { if $0 { model.chartRange = range } }
                    ))
                    .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")))
                }
            }

            Picker("Sort Watchlist By", selection: Binding(get: { model.sort }, set: { model.sort = $0 })) {
                ForEach(WatchlistSort.allCases) { Text($0.name).tag($0) }
            }

            Picker("Show Changes As", selection: Binding(get: { model.changeStyle }, set: { model.changeStyle = $0 })) {
                ForEach(ChangeStyle.allCases, id: \.self) { Text($0.name).tag($0) }
            }

            Divider()
        }

        CommandGroup(replacing: .help) {
            Link("Scryfall (Magic Prices)", destination: DataSource.scryfall.website)
            Link("TCGdex (Pokémon Prices)", destination: DataSource.tcgdex.website)
            Divider()
            Link("CardStock on GitHub", destination: URL(string: "https://github.com/macprotips/PriceTag/tree/main/CardStock")!)
        }
    }
}
