import CardStockKit
import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettings()
                .tabItem { Label("General", systemImage: "gearshape") }
            DataSettings()
                .tabItem { Label("Data", systemImage: "chart.line.uptrend.xyaxis") }
        }
        .frame(width: 480)
    }
}

private struct GeneralSettings: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        Form {
            Picker("Currency", selection: $model.currency) {
                ForEach(Currency.allCases) { currency in
                    Text("\(currency.name) (\(currency.market))").tag(currency)
                }
            }
            Picker("Show changes as", selection: $model.changeStyle) {
                ForEach(ChangeStyle.allCases, id: \.self) { Text($0.name).tag($0) }
            }
            .pickerStyle(.radioGroup)
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
                Text("Price Sources")
            } footer: {
                Text("CardStock only uses free, public APIs that allow apps like this. It doesn't scrape stores. Card history starts the day you add a card.")
                    .foregroundStyle(.secondary)
            }

            Section {
                LabeledContent("Last updated") {
                    if let lastRefresh = model.lastRefresh {
                        Text(lastRefresh, format: .relative(presentation: .named))
                    } else {
                        Text("Never")
                    }
                }
                HStack {
                    Spacer()
                    Button("Refresh Now") { Task { await model.refresh() } }
                        .disabled(model.isRefreshing || model.items.isEmpty)
                }
            }

            Section {
                Text("CardStock isn't affiliated with Wizards of the Coast, Nintendo, The Pokémon Company, TCGplayer, Cardmarket, Scryfall or TCGdex.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .fixedSize(horizontal: false, vertical: true)
    }
}
