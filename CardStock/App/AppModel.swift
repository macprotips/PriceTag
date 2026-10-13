import AppKit
import CardStockKit
import Observation
import UniformTypeIdentifiers
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
        case .change: "Today's Change"
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

    /// Filters the sidebar.
    var searchText = ""
    var gameFilter: Game?

    private(set) var isLoaded = false
    /// Set when the saved watchlist came from a newer CardStock. Nothing is
    /// saved, so that file is never overwritten.
    private(set) var isReadOnly = false
    private(set) var isRefreshing = false
    private(set) var lastRefresh: Date?
    /// Shown under the watchlist when a source couldn't be reached.
    private(set) var refreshProblem: String?

    // MARK: Services

    let refresher = PriceRefresher()
    private let store = WatchlistStore(fileURL: SharedContainer.watchlistURL)
    private let thumbnails = ThumbnailCache()
    @ObservationIgnored private var autoRefresh: Task<Void, Never>?
    /// Saves run one after another, so an older save never lands last.
    @ObservationIgnored private var pendingWrite: Task<Void, Never>?
    /// A widget link that arrived before the watchlist finished loading.
    @ObservationIgnored private var pendingLinkID: WatchItem.ID?

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
            return items
                .map { ($0, $0.currentAmount(in: currency)?.cents ?? Int.min) }
                .sorted { $0.1 > $1.1 }
                .map(\.0)
        case .change:
            return items
                .map { ($0, $0.dayChange(in: currency)?.fraction ?? -.infinity) }
                .sorted { $0.1 > $1.1 }
                .map(\.0)
        }
    }

    /// What the sidebar shows: sorted, then filtered by game and search.
    var visibleItems: [WatchItem] {
        let query = searchText.trimmingCharacters(in: .whitespaces)
        return sortedItems.filter { item in
            (gameFilter == nil || item.card.game == gameFilter)
                && (query.isEmpty
                    || item.card.name.localizedStandardContains(query)
                    || item.card.setName.localizedStandardContains(query)
                    || item.card.setCode.localizedStandardContains(query))
        }
    }

    var isFiltering: Bool { gameFilter != nil || !searchText.trimmingCharacters(in: .whitespaces).isEmpty }

    var collection: CollectionSummary? {
        CollectionSummary.make(from: items, currency: currency)
    }

    func contains(_ quote: CardQuote, finish: Finish) -> Bool {
        items.contains { $0.id == WatchItem.id(cardID: quote.card.id, finish: finish) }
    }

    // MARK: Lifecycle

    func start() async {
        guard !isLoaded else { return }
        await load()
        isLoaded = true
        selection = pendingLinkID.flatMap { id in items.contains { $0.id == id } ? id : nil } ?? selection ?? items.first?.id
        pendingLinkID = nil
        await refreshIfStale(olderThan: launchRefreshAge)
        startAutoRefresh()
    }

    private func load() async {
        do {
            let document = try await store.load()
            items = document.items
            lastRefresh = document.lastRefresh
        } catch let error as WatchlistStoreError {
            isReadOnly = true
            refreshProblem = error.localizedDescription
        } catch {
            // Keep the unreadable file safe before anything can be saved over it.
            if let backup = try? await store.setAsideDamagedFile() {
                refreshProblem = "Your watchlist couldn't be read, so a copy was saved as “\(backup.lastPathComponent)”."
            } else {
                isReadOnly = true
                refreshProblem = "Your watchlist couldn't be read: \(error.localizedDescription)"
            }
        }
    }

    /// Picks up prices the widget saved while the app was in the background,
    /// then refreshes if they're old.
    func becameActive() async {
        guard isLoaded, !isRefreshing, !isReadOnly else { return }
        await pendingWrite?.value
        if let document = try? await store.load(), document.items.map(\.id) == items.map(\.id) {
            items = document.items
            lastRefresh = document.lastRefresh
        }
        await refreshIfStale(olderThan: 6 * 60 * 60)
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

    /// Waits for pending saves. Called before quitting.
    func flush() async {
        await pendingWrite?.value
    }

    // MARK: Actions

    func refresh() async {
        guard isLoaded, !isRefreshing, !items.isEmpty else { return }
        isRefreshing = true
        defer { isRefreshing = false }

        let now = Date()
        let before = items
        let result = await refresher.refresh(items, now: now)
        var document = WatchlistDocument(items: items, lastRefresh: lastRefresh)
        document.merge(refreshed: result.items)
        items = document.items
        // Only say prices are fresh if some actually are.
        if result.updatedCount > 0 { lastRefresh = now }

        if result.failures.isEmpty {
            refreshProblem = isReadOnly ? refreshProblem : nil
        } else if result.failures.values.allSatisfy({ $0 == ProviderError.offline.localizedDescription }) {
            refreshProblem = "You’re offline. Showing the last prices."
        } else {
            let names = result.failures.keys.map(\.name).sorted().formatted(.list(type: .and))
            refreshProblem = "Couldn’t reach \(names). Showing the last prices."
        }

        AlertNotifier.post(PriceAlert.hits(before: before, after: items))
        persist()
        await thumbnails.fetchMissing(for: items.map(\.card))
        persist(watchlist: false)
    }

    /// Adds a card, or selects it if it's already on the watchlist.
    func add(_ quote: CardQuote, finish: Finish) {
        let item = WatchItem(quote: quote, finish: finish)
        searchText = ""
        if gameFilter != nil, gameFilter != item.card.game { gameFilter = nil }
        if !items.contains(where: { $0.id == item.id }) {
            items.append(item)
            persist()
            Task {
                await thumbnails.fetchMissing(for: [item.card])
                persist(watchlist: false)
            }
        }
        selection = item.id
    }

    func remove(_ ids: Set<WatchItem.ID>, undoManager: UndoManager?) {
        let removed = items.enumerated().filter { ids.contains($0.element.id) }
        guard !removed.isEmpty else { return }

        let sorted = visibleItems
        let lastIndex = sorted.lastIndex { ids.contains($0.id) } ?? 0
        let next = sorted.dropFirst(lastIndex + 1).first { !ids.contains($0.id) }
            ?? sorted.prefix(lastIndex).last { !ids.contains($0.id) }

        items.removeAll { ids.contains($0.id) }
        if let selection, ids.contains(selection) { self.selection = next?.id }
        persist()
        thumbnails.removeAll(except: items.map(\.card))

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
        persist()
        Task {
            await thumbnails.fetchMissing(for: removed.map(\.element.card))
            persist(watchlist: false)
        }
        nonisolated(unsafe) let manager = undoManager
        manager?.registerUndo(withTarget: self) { model in
            MainActor.assumeIsolated {
                model.remove(Set(removed.map(\.element.id)), undoManager: manager)
            }
        }
    }

    func move(from source: IndexSet, to destination: Int) {
        guard sort == .manual, !isFiltering else { return }
        items.move(fromOffsets: source, toOffset: destination)
        persist()
    }

    /// Sets how many copies you own and what you paid. Nil or zero clears it.
    func setHolding(_ holding: Holding?, for id: WatchItem.ID) {
        guard let index = items.firstIndex(where: { $0.id == id }) else { return }
        let cleaned = holding.flatMap { $0.quantity > 0 ? $0 : nil }
        guard items[index].holding != cleaned else { return }
        items[index].holding = cleaned
        persist()
    }

    func setAlert(_ alert: PriceAlert?, for id: WatchItem.ID) {
        guard let index = items.firstIndex(where: { $0.id == id }) else { return }
        let cleaned = alert.flatMap { $0.isEmpty ? nil : $0 }
        guard items[index].alert != cleaned else { return }
        items[index].alert = cleaned
        persist(widget: false)
        if cleaned != nil { AlertNotifier.requestPermission() }
    }

    func open(_ url: URL) {
        guard let id = DeepLink.itemID(from: url) else { return }
        guard isLoaded else {
            // The watchlist is still loading; select it once it's there.
            pendingLinkID = id
            return
        }
        if items.contains(where: { $0.id == id }) {
            searchText = ""
            gameFilter = nil
            selection = id
        }
    }

    // MARK: Copy and export

    /// "Sheoldred, the Apocalypse (DMU · Foil): $84.20"
    func clipboardText(for ids: Set<WatchItem.ID>) -> String {
        visibleItems
            .filter { ids.contains($0.id) }
            .map { "\($0.card.name) (\($0.subtitle)): \(PriceFormat.price($0.currentAmount(in: currency), currency))" }
            .joined(separator: "\n")
    }

    func copy(_ ids: Set<WatchItem.ID>) {
        let text = clipboardText(for: ids)
        guard !text.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    func exportCSV() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.nameFieldStringValue = "CardStock Watchlist.csv"
        panel.canCreateDirectories = true
        let csv = WatchlistCSV.make(from: sortedItems, currency: currency)
        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }
            try? Data(csv.utf8).write(to: url, options: .atomic)
        }
    }

    // MARK: Saving

    /// Saves the watchlist and refreshes the widget, in order. Writes queue up
    /// behind each other so the newest state is always the one that lands.
    private func persist(watchlist: Bool = true, widget: Bool = true) {
        guard !isReadOnly else { return }
        let document = WatchlistDocument(items: items, lastRefresh: lastRefresh)
        let snapshot = widget ? WidgetSnapshot.make(from: document, currency: currency) : nil
        let previous = pendingWrite
        pendingWrite = Task { [store, weak self] in
            await previous?.value
            if watchlist {
                do {
                    try await store.save(document)
                } catch {
                    self?.refreshProblem = "Couldn’t save your watchlist: \(error.localizedDescription)"
                }
            }
            if let snapshot {
                try? snapshot.write()
                WidgetCenter.shared.reloadAllTimelines()
            }
        }
    }

    private func publishToWidget() {
        persist(watchlist: false)
    }
}
