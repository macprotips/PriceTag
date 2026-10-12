import CardStockKit
import SwiftUI

/// Search state for the Add Card sheet.
@MainActor
@Observable
final class CardSearch {
    var query = ""
    var game: Game = Game(rawValue: UserDefaults.standard.string(forKey: "searchGame") ?? "") ?? .magic {
        didSet { UserDefaults.standard.set(game.rawValue, forKey: "searchGame") }
    }

    private(set) var results: [CardQuote] = []
    private(set) var isSearching = false
    private(set) var error: String?
    /// The query the current results are for.
    private(set) var searchedQuery = ""

    private let refresher: PriceRefresher

    init(refresher: PriceRefresher) {
        self.refresher = refresher
    }

    func search() async {
        let query = self.query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty, let provider = refresher.provider(for: game) else {
            results = []
            searchedQuery = ""
            error = nil
            return
        }
        isSearching = true
        defer { isSearching = false }
        do {
            let found = try await provider.search(query)
            try Task.checkCancellation()
            results = found
            searchedQuery = query
            error = nil
        } catch is CancellationError {
            // A newer search replaced this one.
        } catch {
            results = []
            searchedQuery = query
            self.error = error.localizedDescription
        }
    }

    /// Search results from TCGdex only have the basics. Fetch the rest when a
    /// row appears.
    func complete(_ quote: CardQuote) async {
        guard !quote.isComplete, let provider = refresher.provider(for: quote.card.game),
              let full = try? await provider.quote(for: quote.card.sourceID),
              let index = results.firstIndex(where: { $0.id == quote.id })
        else { return }
        results[index] = full
    }
}

struct AddCardSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    @State private var search = CardSearch(refresher: PriceRefresher())
    /// Stops Return in the search field and the default button from adding twice.
    @State private var isAdding = false
    @State private var selection: CardQuote.ID?
    /// The finish picked for each result, when it isn't the suggested one.
    @State private var finishes: [CardQuote.ID: Finish] = [:]
    @FocusState private var searchFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            SearchHeader(search: search, focused: $searchFocused, onMove: moveSelection, onSubmit: addSelected)
            Divider()
            results(search)
            Divider()
            bottomBar(search)
        }
        .frame(width: 600, height: 560)
        .onAppear { searchFocused = true }
        .task(id: searchKey) {
            // Wait for a pause in typing.
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            await search.search()
            selection = search.results.first?.id
        }
    }

    private var searchKey: String {
        "\(search.game.rawValue):\(search.query)"
    }

    @ViewBuilder
    private func results(_ search: CardSearch) -> some View {
        if search.query.trimmingCharacters(in: .whitespaces).isEmpty {
            ContentUnavailableView {
                Label("Find a Card", systemImage: "magnifyingglass")
            } description: {
                Text(search.game == .magic
                     ? "Type a card name. Scryfall search terms work too, like “sol ring set:cmm”."
                     : "Type a Pokémon card name, like “Charizard”.")
            }
        } else if let error = search.error {
            ContentUnavailableView {
                Label("Couldn't Search", systemImage: "wifi.exclamationmark")
            } description: {
                Text(error)
            } actions: {
                Button("Try Again") { Task { await search.search() } }
            }
        } else if search.results.isEmpty && !search.isSearching && search.searchedQuery == search.query.trimmingCharacters(in: .whitespaces) {
            ContentUnavailableView.search(text: search.query)
        } else {
            ScrollViewReader { proxy in
                List(search.results, selection: $selection) { quote in
                    SearchResultRow(
                        quote: quote,
                        finish: finishBinding(for: quote),
                        isAdded: model.contains(quote, finish: finish(for: quote))
                    )
                    .tag(quote.id)
                    .task { await search.complete(quote) }
                }
                .listStyle(.inset)
                // Double-clicking a result adds it.
                .contextMenu(forSelectionType: CardQuote.ID.self) { _ in
                    EmptyView()
                } primaryAction: { ids in
                    guard let id = ids.first else { return }
                    selection = id
                    addSelected()
                }
                .overlay {
                    if search.isSearching && search.results.isEmpty {
                        ProgressView()
                    }
                }
                .onChange(of: selection) { _, id in
                    if let id { proxy.scrollTo(id) }
                }
            }
        }
    }

    private func bottomBar(_ search: CardSearch) -> some View {
        HStack {
            Text("Cards and prices from \(search.game.source.name)")
                .font(.footnote)
                .foregroundStyle(.secondary)
            Spacer()
            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Button(selectedIsAdded ? "Show Card" : "Add Card") { addSelected() }
                .keyboardShortcut(.defaultAction)
                .disabled(selectedQuote == nil || isAdding)
        }
        .padding(16)
    }

    // MARK: Selection

    private var selectedQuote: CardQuote? {
        guard let selection else { return nil }
        return search.results.first { $0.id == selection }
    }

    private var selectedIsAdded: Bool {
        guard let quote = selectedQuote else { return false }
        return model.contains(quote, finish: finish(for: quote))
    }

    private func finish(for quote: CardQuote) -> Finish {
        finishes[quote.id] ?? quote.suggestedFinish(for: model.currency) ?? quote.card.finishes.first ?? .normal
    }

    private func finishBinding(for quote: CardQuote) -> Binding<Finish> {
        Binding(get: { finish(for: quote) }, set: { finishes[quote.id] = $0 })
    }

    private func moveSelection(by offset: Int) {
        let results = search.results
        guard !results.isEmpty else { return }
        let current = results.firstIndex { $0.id == selection } ?? (offset > 0 ? -1 : results.count)
        selection = results[max(0, min(results.count - 1, current + offset))].id
    }

    private func addSelected() {
        guard let quote = selectedQuote, !isAdding else { return }
        isAdding = true
        Task {
            // Make sure we save the full details, not a bare search result.
            var full = quote
            if !quote.isComplete, let provider = model.refresher.provider(for: quote.card.game) {
                full = (try? await provider.quote(for: quote.card.sourceID)) ?? quote
            }
            model.add(full, finish: finish(for: full))
            dismiss()
        }
    }
}

/// The game picker and search field.
private struct SearchHeader: View {
    @Bindable var search: CardSearch
    var focused: FocusState<Bool>.Binding
    var onMove: (Int) -> Void
    var onSubmit: () -> Void

    var body: some View {
        VStack(spacing: 12) {
            Picker("Game", selection: $search.game) {
                ForEach(Game.allCases) { Text($0.name).tag($0) }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(width: 220)

            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                TextField("Search \(search.game.name) Cards", text: $search.query)
                    .textFieldStyle(.plain)
                    .font(.title3)
                    .focused(focused)
                    .onSubmit(onSubmit)
                    .onKeyPress(.downArrow) { onMove(1); return .handled }
                    .onKeyPress(.upArrow) { onMove(-1); return .handled }
                if search.isSearching {
                    ProgressView().controlSize(.small)
                } else if !search.query.isEmpty {
                    Button {
                        search.query = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.tertiary)
                    }
                    .buttonStyle(.plain)
                    .help("Clear")
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 9)
            .background(RoundedRectangle(cornerRadius: 9, style: .continuous).fill(.quinary))
        }
        .padding(16)
    }
}

private struct SearchResultRow: View {
    @Environment(AppModel.self) private var model
    var quote: CardQuote
    @Binding var finish: Finish
    var isAdded: Bool

    var body: some View {
        HStack(spacing: 12) {
            CardArtwork(url: quote.card.thumbnailURL, width: 40)

            VStack(alignment: .leading, spacing: 3) {
                Text(quote.card.name)
                    .font(.headline)
                    .lineLimit(1)
                Text(quote.isComplete ? "\(quote.card.setName) · #\(quote.card.number)" : "\(quote.card.setCode) · #\(quote.card.number)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer(minLength: 8)

            if quote.card.finishes.count > 1 {
                Picker("Finish", selection: $finish) {
                    ForEach(quote.card.finishes) { Text($0.name).tag($0) }
                }
                .labelsHidden()
                .fixedSize()
            } else if let only = quote.card.finishes.first {
                Text(only.name)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Group {
                if !quote.isComplete {
                    ProgressView().controlSize(.small)
                } else {
                    Text(PriceFormat.price(quote.price(for: finish)?.amount(in: model.currency), model.currency))
                        .monospacedDigit()
                }
            }
            .frame(minWidth: 72, alignment: .trailing)

            Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.secondary)
                .opacity(isAdded ? 1 : 0)
                .help("On your watchlist")
        }
        .padding(.vertical, 4)
    }
}
