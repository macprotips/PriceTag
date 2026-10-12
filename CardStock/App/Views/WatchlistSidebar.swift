import CardStockKit
import SwiftUI

struct WatchlistSidebar: View {
    @Environment(AppModel.self) private var model
    @Environment(\.undoManager) private var undoManager

    var body: some View {
        @Bindable var model = model

        List(selection: $model.selection) {
            ForEach(model.sortedItems) { item in
                WatchlistRow(item: item)
                    .tag(item.id)
            }
            .onMove(perform: moveAction)
        }
        .listStyle(.sidebar)
        .navigationTitle("Watchlist")
        .onDeleteCommand {
            if let selection = model.selection {
                model.remove([selection], undoManager: undoManager)
            }
        }
        .contextMenu(forSelectionType: WatchItem.ID.self) { ids in
            if let id = ids.first, let item = model.items.first(where: { $0.id == id }), ids.count == 1 {
                ForEach(item.card.links, id: \.url) { link in
                    Link("Open on \(link.title)", destination: link.url)
                }
                if !item.card.links.isEmpty { Divider() }
            }
            Button(ids.count > 1 ? "Remove \(ids.count) Cards" : "Remove from Watchlist", role: .destructive) {
                model.remove(ids, undoManager: undoManager)
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            StatusFooter()
        }
    }

    /// Dragging to reorder only makes sense when sorted manually.
    private var moveAction: ((IndexSet, Int) -> Void)? {
        guard model.sort == .manual else { return nil }
        return { model.move(from: $0, to: $1) }
    }
}

/// "Updated 2 hours ago", or what went wrong, at the foot of the sidebar.
private struct StatusFooter: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Group {
            if model.isRefreshing {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.mini)
                    Text("Updating Prices…")
                }
            } else if let problem = model.refreshProblem {
                Label(problem, systemImage: "exclamationmark.triangle.fill")
                    .labelStyle(StatusLabelStyle())
            } else if let lastRefresh = model.lastRefresh {
                TimelineView(.periodic(from: .now, by: 60)) { _ in
                    Text("Updated \(lastRefresh, format: .relative(presentation: .named))")
                }
            }
        }
        .font(.footnote)
        .foregroundStyle(.secondary)
        .lineLimit(2)
        .multilineTextAlignment(.center)
        .frame(maxWidth: .infinity)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
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
        let month = item.series(in: currency, range: .month)

        HStack(spacing: 10) {
            CardArtwork(url: item.card.thumbnailURL, width: 28)

            VStack(alignment: .leading, spacing: 2) {
                Text(item.card.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(item.subtitle)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 6)

            Sparkline(values: item.sparkline(in: currency), direction: month.change.direction)
                .frame(width: 44, height: 20)

            VStack(alignment: .trailing, spacing: 3) {
                Text(PriceFormat.price(item.currentAmount(in: currency), currency))
                    .font(.body.weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(item.isStale() ? .secondary : .primary)
                Button {
                    model.changeStyle = model.changeStyle == .percent ? .amount : .percent
                } label: {
                    ChangeBadge(change: change, currency: currency, asPercent: model.changeStyle == .percent)
                }
                .buttonStyle(.plain)
                .help("Show changes as \(model.changeStyle == .percent ? "amounts" : "percentages")")
            }
        }
        .padding(.vertical, 4)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(accessibilityLabel(change: change, currency: currency))
    }

    private func accessibilityLabel(change: PriceChange?, currency: Currency) -> String {
        "\(item.card.name), \(item.card.setName) \(item.finish.name), "
            + "\(PriceFormat.price(item.currentAmount(in: currency), currency)), "
            + PriceFormat.spokenChange(change, currency)
    }
}
