import CardStockKit
import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettings()
                .tabItem { Label("General", systemImage: "gearshape") }
            DataSettings()
                .tabItem { Label("Prices", systemImage: "chart.line.uptrend.xyaxis") }
        }
        .frame(width: 500)
        .scenePadding(.minimum, edges: .bottom)
    }
}

private struct GeneralSettings: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        Form {
            Section {
                Picker("Currency", selection: $model.currency) {
                    ForEach(Currency.allCases) { currency in
                        Text("\(currency.name) (\(currency.code))").tag(currency)
                    }
                }
                Picker("Show changes as", selection: $model.changeStyle) {
                    ForEach(ChangeStyle.allCases, id: \.self) { Text($0.name).tag($0) }
                }
            } footer: {
                Text("Dollar prices come from TCGplayer and euro prices from Cardmarket, for both games.")
            }
        }
        .formStyle(.grouped)
        .fixedSize(horizontal: false, vertical: true)
    }
}

private struct DataSettings: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Form {
            Section {
                ForEach(DataSource.allCases) { source in
                    LabeledContent {
                        Link("Website", destination: source.website)
                    } label: {
                        Text(source.name)
                        Text(source.summary)
                    }
                }
            } header: {
                Text("Sources")
            } footer: {
                Text("CardStock only uses free, public price APIs that allow apps like this, and never scrapes stores. Each card’s history starts the day you add it.")
            }

            Section {
                LabeledContent("Last updated") {
                    HStack(spacing: 10) {
                        if model.isRefreshing {
                            ProgressView().controlSize(.small)
                        } else if let lastRefresh = model.lastRefresh {
                            Text(lastRefresh, format: .relative(presentation: .named))
                        } else {
                            Text("Never")
                        }
                        Button("Refresh Now") { Task { await model.refresh() } }
                            .disabled(model.isRefreshing || model.items.isEmpty)
                    }
                }
            } footer: {
                Text("CardStock isn’t affiliated with Wizards of the Coast, Nintendo, The Pokémon Company, TCGplayer, Cardmarket, Scryfall or TCGdex.")
            }
        }
        .formStyle(.grouped)
        .fixedSize(horizontal: false, vertical: true)
    }
}
