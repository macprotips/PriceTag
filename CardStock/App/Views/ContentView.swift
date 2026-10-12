import AppKit
import CardStockKit
import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationSplitView {
            WatchlistSidebar()
                .navigationSplitViewColumnWidth(min: 270, ideal: 310, max: 400)
        } detail: {
            detail
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button {
                    Task { await model.refresh() }
                } label: {
                    Label("Refresh Prices", systemImage: "arrow.clockwise")
                }
                .help("Refresh prices (⌘R)")
                .disabled(model.isRefreshing || model.items.isEmpty)

                Button {
                    model.isAddingCard = true
                } label: {
                    Label("Add Card", systemImage: "plus")
                }
                .help("Add a card (⌘N)")
            }
        }
        .sheet(isPresented: $model.isAddingCard) {
            AddCardSheet()
        }
        .task { await model.start() }
        .onOpenURL { model.open($0) }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            Task { await model.refreshIfStale(olderThan: 6 * 60 * 60) }
        }
    }

    @ViewBuilder
    private var detail: some View {
        if let item = model.selectedItem {
            CardDetailView(item: item)
                .id(item.id)
        } else if model.isLoaded && model.items.isEmpty {
            ContentUnavailableView {
                Label("No Cards", systemImage: "rectangle.portrait.on.rectangle.portrait")
            } description: {
                Text("Add Magic and Pokémon cards to follow their prices.")
            } actions: {
                Button("Add Card") { model.isAddingCard = true }
                    .controlSize(.large)
                    .buttonStyle(.borderedProminent)
            }
        } else if model.isLoaded {
            ContentUnavailableView("No Card Selected", systemImage: "rectangle.portrait")
        }
    }
}
