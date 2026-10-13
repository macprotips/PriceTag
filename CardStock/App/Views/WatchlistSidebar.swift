import CardStockKit
import SwiftUI

/// Filters for the sidebar search.
enum GameScope: Hashable {
    case all
    case game(Game)

    var game: Game? {
        if case .game(let game) = self { return game }
        return nil
    }
}

struct WatchlistSidebar: View {
    @Environment(AppModel.self) private var model
    @Environment(\.undoManager) private var undoManager

    var body: some View {
        @Bindable var model = model
        let items = model.visibleItems

        List(selection: $model.selection) {
            if let collection = model.collection, !model.isFiltering {
                CollectionSummaryView(summary: collection, currency: model.currency)
                    .selectionDisabled()
                    .listRowSeparator(.hidden)
            }

            Section {
                ForEach(items) { item in
                    WatchlistRow(item: item)
                        .tag(item.id)
                }
                .onMove(perform: moveAction)
            } header: {
                if !model.items.isEmpty {
                    SidebarHeader()
                }
            }
        }
        .listStyle(.sidebar)
        .searchable(text: $model.searchText, placement: .sidebar, prompt: "Search Watchlist")
        .searchScopes(scopeBinding) {
            Text("All").tag(GameScope.all)
            ForEach(Game.allCases) { Text($0.name).tag(GameScope.game($0)) }
        }
        .overlay {
            if model.isFiltering && items.isEmpty {
                ContentUnavailableView.search(text: model.searchText)
            }
        }
        .onDeleteCommand {
            if let selection = model.selection {
                model.remove([selection], undoManager: undoManager)
            }
        }
        .onCopyCommand {
            guard let selection = model.selection else { return [] }
            return [NSItemProvider(object: model.clipboardText(for: [selection]) as NSString)]
        }
        .contextMenu(forSelectionType: WatchItem.ID.self) { ids in
            if let id = ids.first, ids.count == 1, let item = model.items.first(where: { $0.id == id }) {
                ForEach(item.card.links, id: \.url) { link in
                    Link("Open on \(link.title)", destination: link.url)
                }
                Divider()
            }
            Button("Copy") { model.copy(ids) }
            Divider()
            Button(ids.count > 1 ? "Remove \(ids.count) Cards" : "Remove from Watchlist", role: .destructive) {
                model.remove(ids, undoManager: undoManager)
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            StatusFooter()
        }
    }

    private var scopeBinding: Binding<GameScope> {
        Binding(
            get: { model.gameFilter.map(GameScope.game) ?? .all },
            set: { model.gameFilter = $0.game }
        )
    }

    /// Dragging to reorder only makes sense when sorted manually and unfiltered.
    private var moveAction: ((IndexSet, Int) -> Void)? {
        guard model.sort == .manual, !model.isFiltering else { return nil }
        return { model.move(from: $0, to: $1) }
    }
}

/// "Watchlist" with the sort menu, like a Finder sidebar section.
private struct SidebarHeader: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        HStack {
            Text("Watchlist")
            Spacer()
            Menu {
                Picker("Sort By", selection: $model.sort) {
                    ForEach(WatchlistSort.allCases) { Text($0.name).tag($0) }
                }
                .pickerStyle(.inline)
            } label: {
                Image(systemName: "arrow.up.arrow.down")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .help("Sort Watchlist")
        }
    }
}

/// The value of the cards you own, at the top of the sidebar.
private struct CollectionSummaryView: View {
    @Environment(AppModel.self) private var model
    var summary: CollectionSummary
    var currency: Currency

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("My Collection")
                .font(.subheadline.weight(.medium))
                .foregroundStyle(.secondary)

            HStack(alignment: .firstTextBaseline) {
                Text(PriceFormat.price(summary.value, currency))
                    .font(.system(size: 26, weight: .semibold))
                    .monospacedDigit()
                    .contentTransition(.numericText())
                Spacer(minLength: 8)
                Sparkline(values: summary.history.map(\.value), direction: summary.dayChange.direction)
                    .frame(width: 64, height: 24)
            }

            HStack(spacing: 6) {
                if let change = summary.dayChange {
                    Text(changeText(change))
                        .foregroundStyle(change.direction.color)
                    Text("today")
                        .foregroundStyle(.secondary)
                } else {
                    Text(summary.cardCount == 1 ? "1 card" : "\(summary.cardCount) cards")
                        .foregroundStyle(.secondary)
                }
            }
            .font(.callout.weight(.medium))
            .monospacedDigit()

            if let gain = summary.gain {
                HStack(spacing: 6) {
                    Text(changeText(gain))
                        .foregroundStyle(gain.direction.color)
                    Text("since bought")
                        .foregroundStyle(.secondary)
                }
                .font(.callout.weight(.medium))
                .monospacedDigit()
            }
        }
        .padding(.vertical, 8)
        .padding(.horizontal, 4)
        .accessibilityElement(children: .combine)
    }

    private func changeText(_ change: PriceChange) -> String {
        let amount = PriceFormat.change(change.amount, currency)
        guard let fraction = change.fraction else { return amount }
        return "\(amount) (\(PriceFormat.percent(fraction)))"
    }
}

/// "Updated 2 hours ago", or what went wrong, at the foot of the sidebar.
private struct StatusFooter: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            Divider()
            Group {
                if model.isRefreshing {
                    HStack(spacing: 6) {
                        ProgressView().controlSize(.mini)
                        Text("Updating prices…")
                    }
                } else if let problem = model.refreshProblem {
                    Label(problem, systemImage: "exclamationmark.triangle.fill")
                        .labelStyle(StatusLabelStyle())
                } else if let lastRefresh = model.lastRefresh {
                    TimelineView(.periodic(from: .now, by: 60)) { _ in
                        Text("Updated \(lastRefresh, format: .relative(presentation: .named))")
                    }
                } else {
                    Text(model.items.isEmpty ? "" : "Not updated yet")
                }
            }
            .font(.footnote)
            .foregroundStyle(.secondary)
            .lineLimit(2)
            .multilineTextAlignment(.center)
            .frame(maxWidth: .infinity, minHeight: 16)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
        }
        .background(.bar)
    }
}

private struct StatusLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 4) {
            configuration.icon.foregroundStyle(.yellow)
            configuration.title
        }
    }
}

struct WatchlistRow: View {
    @Environment(AppModel.self) private var model
    var item: WatchItem

    var body: some View {
        let currency = model.currency
        let change = item.dayChange(in: currency)
        let sparkline = item.sparkline(in: currency)
        let isOld = item.isStale() || !item.hasCurrentPrice(in: currency)

        HStack(spacing: 10) {
            CardArtwork(url: item.card.thumbnailURL, width: 30)

            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text(item.card.name)
                        .font(.body.weight(.semibold))
                        .lineLimit(1)
                    if item.alert != nil {
                        Image(systemName: "bell.fill")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .help("Price alert set")
                    }
                }
                Text(subtitle)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 6)

            Sparkline(values: sparkline, direction: rangeDirection(sparkline))
                .frame(width: 46, height: 22)

            VStack(alignment: .trailing, spacing: 3) {
                Text(PriceFormat.price(item.currentAmount(in: currency), currency))
                    .font(.body.weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(isOld ? .secondary : .primary)
                    .contentTransition(.numericText())
                Button {
                    model.changeStyle = model.changeStyle == .percent ? .amount : .percent
                } label: {
                    ChangeBadge(change: change, currency: currency, asPercent: model.changeStyle == .percent)
                }
                .buttonStyle(.plain)
                .help(model.changeStyle == .percent ? "Show Changes as Amounts" : "Show Changes as Percentages")
            }
        }
        .padding(.vertical, 5)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(accessibilityLabel(change: change, currency: currency, isOld: isOld))
        .accessibilityAction(named: model.changeStyle == .percent ? "Show Changes as Amounts" : "Show Changes as Percentages") {
            model.changeStyle = model.changeStyle == .percent ? .amount : .percent
        }
    }

    /// "DMU · Foil · 2 owned"
    private var subtitle: String {
        guard let quantity = item.holding?.quantity, quantity > 0 else { return item.subtitle }
        return "\(item.subtitle) · \(quantity) owned"
    }

    private func rangeDirection(_ values: [Double]) -> PriceChange.Direction {
        guard let first = values.first, let last = values.last else { return .flat }
        return last > first ? .up : last < first ? .down : .flat
    }

    private func accessibilityLabel(change: PriceChange?, currency: Currency, isOld: Bool) -> String {
        var parts = [
            item.card.name,
            "\(item.card.setName) \(item.finish.name)",
            PriceFormat.price(item.currentAmount(in: currency), currency),
            PriceFormat.spokenChange(change, currency),
        ]
        if isOld { parts.append("price may be out of date") }
        if let quantity = item.holding?.quantity { parts.append("\(quantity) owned") }
        if item.alert != nil { parts.append("alert set") }
        return parts.joined(separator: ", ")
    }
}
