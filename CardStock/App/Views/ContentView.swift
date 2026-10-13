import AppKit
import CardStockKit
import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationSplitView {
            WatchlistSidebar()
                .navigationSplitViewColumnWidth(min: 280, ideal: 320, max: 420)
        } detail: {
            detail
        }
        .navigationTitle("CardStock")
        .modifier(HiddenWindowTitle())
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button {
                    Task { await model.refresh() }
                } label: {
                    Label("Refresh Prices", systemImage: "arrow.clockwise")
                }
                .help("Refresh Prices")
                .disabled(model.isRefreshing || model.items.isEmpty)

                Button {
                    model.isAddingCard = true
                } label: {
                    Label("Add Card", systemImage: "plus")
                }
                .help("Add Card")
            }
        }
        .sheet(isPresented: $model.isAddingCard) {
            AddCardSheet()
        }
        .task { await model.start() }
        .onOpenURL { model.open($0) }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            Task { await model.becameActive() }
        }
    }

    @ViewBuilder
    private var detail: some View {
        if let item = model.selectedItem {
            CardDetailView(item: item)
                .id(item.id)
        } else if model.isLoaded && model.items.isEmpty {
            ContentUnavailableView {
                Label("Start Your Watchlist", systemImage: "rectangle.portrait.on.rectangle.portrait")
            } description: {
                Text("Add Magic and Pokémon cards to follow their prices, like stocks.")
            } actions: {
                Button("Add Card") { model.isAddingCard = true }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
            }
        } else if model.isLoaded {
            ContentUnavailableView("No Card Selected", systemImage: "rectangle.portrait",
                                   description: Text("Choose a card from your watchlist."))
        }
    }
}

/// The card's name is already the detail view's headline, so the toolbar
/// doesn't repeat it. The window keeps its title for the Window menu.
private struct HiddenWindowTitle: ViewModifier {
    func body(content: Content) -> some View {
        if #available(macOS 15.0, *) {
            content.toolbar(removing: .title)
        } else {
            content
        }
    }
}
