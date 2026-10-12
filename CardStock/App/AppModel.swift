import AppKit
import CardStockKit
import Observation
import SwiftUI
import WidgetKit

/// How the change pills read. Clicking any pill switches all of them, like Stocks.
enum ChangeStyle: String, CaseIterable {
    case percent
    case amount

    var name: String {
        switch self {
        case .percent: "Percentage"
        case .amount: "Amount"
        }
    }
}

enum WatchlistSort: String, CaseIterable, Identifiable {
    case manual
    case name
    case price
    case change

    var id: String { rawValue }

    var name: String {
        switch self {
        case .manual: "Manually"
        case .name: "Name"
        case .price: "Price"
        case .change: "Change"
        }
    }
}

@MainActor
@Observable
final class AppModel {
    // MARK: State

    private(set) var items: [WatchItem] = []
    var selection: WatchItem.ID?
    var isAddingCard = false

    var currency: Currency = Preferences.currency {
        didSet {
            Preferences.currency = currency
            publishToWidget()
        }
    }

    var chartRange: ChartRange = ChartRange(rawValue: UserDefaults.standard.string(forKey: Keys.chartRange) ?? "") ?? .threeMonths {
        didSet { UserDefaults.standard.set(chartRange.rawValue, forKey: Keys.chartRange) }
    }

    var changeStyle: ChangeStyle = ChangeStyle(rawValue: UserDefaults.standard.string(forKey: Keys.changeStyle) ?? "") ?? .percent {
        didSet { UserDefaults.standard.set(changeStyle.rawValue, forKey: Keys.changeStyle) }
    }

    var sort: WatchlistSort = WatchlistSort(rawValue: UserDefaults.standard.string(forKey: Keys.sort) ?? "") ?? .manual {
        didSet { UserDefaults.standard.set(sort.rawValue, forKey: Keys.sort) }
    }

    private(set) var isLoaded = false
    private(set) var isRefreshing = false
    private(set) var lastRefresh: Date?
    /// Shown under the watchlist when a source couldn't be reached.
    private(set) var refreshProblem: String?

    // MARK: Services

    let refresher = PriceRefresher()
    private let store = WatchlistStore(fileURL: SharedContainer.watchlistURL)
    private let thumbnails = ThumbnailCache()
    @ObservationIgnored private var autoRefresh: Task<Void, Never>?

    /// Refresh on launch if prices are older than this.
    private let launchRefreshAge: TimeInterval = 60 * 60
    /// Then refresh this often while the app runs. Sources update daily.
    private let autoRefreshInterval: Duration = .seconds(6 * 60 * 60)

    private enum Keys {
        static let chartRange = "chartRange"
        static let changeStyle = "changeStyle"
        static let sort = "sort"
    }

    // MARK: Derived

    var selectedItem: WatchItem? {
        guard let selection else { return nil }
        return items.first { $0.id == selection }
    }

    var sortedItems: [WatchItem] {
        switch sort {
        case .manual:
            return items
        case .name:
            return items.sorted { $0.card.name.localizedStandardCompare($1.card.name) == .orderedAscending }
        case .price:
            return items.sorted { ($0.currentAmount(in: currency) ?? .zero) > ($1.currentAmount(in: currency) ?? .zero) }
        case .change:
            return items.sorted {
                ($0.dayChange(in: currency)?.fraction ?? -.infinity) > ($1.dayChange(in: currency)?.fraction ?? -.infinity)
            }
        }
    }

    func contains(_ quote: CardQuote, finish: Finish) -> Bool {
        items.contains { $0.id == WatchItem.id(cardID: quote.card.id, finish: finish) }
    }

    // MARK: Lifecycle

    func start() async {
        guard !isLoaded else { return }
        do {
            let document = try await store.load()
            items = document.items
            lastRefresh = document.lastRefresh
        } catch {
            refreshProblem = "Couldn't open your watchlist: \(error.localizedDescription)"
        }
        isLoaded = true
        selection = selection ?? items.first?.id
        await refreshIfStale(olderThan: launchRefreshAge)
        startAutoRefresh()
    }

    func refreshIfStale(olderThan age: TimeInterval) async {
        if let lastRefresh, Date().timeIntervalSince(lastRefresh) < age { return }
        await refresh()
    }

    private func startAutoRefresh() {
        autoRefresh?.cancel()
        autoRefresh = Task { [weak self, autoRefreshInterval] in
            while !Task.isCancelled {
                try? await Task.sleep(for: autoRefreshInterval)
                guard !Task.isCancelled else { return }
                await self?.refresh()
            }
        }
    }

    // MARK: Actions

    func refresh() async {
        guard isLoaded, !isRefreshing, !items.isEmpty else { return }
        isRefreshing = true
        defer { isRefreshing = false }

        let now = Date()
        let result = await refresher.refresh(items, now: now)
        var document = WatchlistDocument(items: items, lastRefresh: now)
        document.merge(refreshed: result.items)
        items = document.items
        lastRefresh = now

        if result.failures.isEmpty {
            refreshProblem = nil
        } else if result.failures.values.allSatisfy({ $0 == ProviderError.offline.localizedDescription }) {
            refreshProblem = "You're offline. Showing the last prices."
        } else {
            let names = result.failures.keys.map(\.name).sorted().formatted(.list(type: .and))
            refreshProblem = "Couldn't reach \(names). Showing the last prices."
        }

        save()
        await thumbnails.fetchMissing(for: items.map(\.card))
        publishToWidget()
    }

    /// Adds a card, or selects it if it's already on the watchlist.
    func add(_ quote: CardQuote, finish: Finish) {
        let item = WatchItem(quote: quote, finish: finish)
        if !items.contains(where: { $0.id == item.id }) {
            items.append(item)
            save()
            Task {
                await thumbnails.fetchMissing(for: [item.card])
                publishToWidget()
            }
        }
        selection = item.id
    }

    func remove(_ ids: Set<WatchItem.ID>, undoManager: UndoManager?) {
        let removed = items.enumerated().filter { ids.contains($0.element.id) }
        guard !removed.isEmpty else { return }

        let sorted = sortedItems
        let lastIndex = sorted.lastIndex { ids.contains($0.id) } ?? 0
        let next = sorted.dropFirst(lastIndex + 1).first { !ids.contains($0.id) }
            ?? sorted.prefix(lastIndex).last { !ids.contains($0.id) }

        items.removeAll { ids.contains($0.id) }
        if let selection, ids.contains(selection) { self.selection = next?.id }
        save()
        thumbnails.removeAll(except: items.map(\.card))
        publishToWidget()

        // UndoManager isn't Sendable, but undo always runs on the main thread.
        nonisolated(unsafe) let manager = undoManager
        manager?.registerUndo(withTarget: self) { model in
            MainActor.assumeIsolated {
                model.restore(removed, undoManager: manager)
            }
        }
        undoManager?.setActionName(removed.count == 1 ? "Remove Card" : "Remove Cards")
    }

    private func restore(_ removed: [(offset: Int, element: WatchItem)], undoManager: UndoManager?) {
        for (offset, item) in removed.sorted(by: { $0.offset < $1.offset }) {
            items.insert(item, at: min(offset, items.count))
        }
        selection = removed.first?.element.id
        save()
        Task {
            await thumbnails.fetchMissing(for: removed.map(\.element.card))
            publishToWidget()
        }
        nonisolated(unsafe) let manager = undoManager
        manager?.registerUndo(withTarget: self) { model in
            MainActor.assumeIsolated {
                model.remove(Set(removed.map(\.element.id)), undoManager: manager)
            }
        }
    }

    func move(from source: IndexSet, to destination: Int) {
        guard sort == .manual else { return }
        items.move(fromOffsets: source, toOffset: destination)
        save()
        publishToWidget()
    }

    func open(_ url: URL) {
        guard let id = DeepLink.itemID(from: url), items.contains(where: { $0.id == id }) else { return }
        selection = id
    }

    // MARK: Saving

    private func save() {
        let document = WatchlistDocument(items: items, lastRefresh: lastRefresh)
        Task {
            do {
                try await store.save(document)
            } catch {
                refreshProblem = "Couldn't save: \(error.localizedDescription)"
            }
        }
    }

    /// Writes the small file the widget reads and asks it to redraw.
    private func publishToWidget() {
        let snapshot = WidgetSnapshot.make(from: WatchlistDocument(items: items, lastRefresh: lastRefresh), currency: currency)
        Task.detached(priority: .utility) {
            try? snapshot.write()
            WidgetCenter.shared.reloadAllTimelines()
        }
    }
}
