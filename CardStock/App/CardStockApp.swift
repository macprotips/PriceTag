import AppKit
import CardStockKit
import SwiftUI
import UserNotifications

@main
struct CardStockApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var model = AppModel()

    init() {
        // Card images are fetched with AsyncImage, which uses the shared cache.
        URLCache.shared = URLCache(memoryCapacity: 64 * 1024 * 1024, diskCapacity: 512 * 1024 * 1024)
    }

    var body: some Scene {
        Window("CardStock", id: "main") {
            ContentView()
                .environment(model)
                .frame(minWidth: 880, minHeight: 580)
                .onAppear { appDelegate.model = model }
        }
        .defaultSize(width: 1180, height: 760)
        .commands {
            SidebarCommands()
            CardStockCommands(model: model)
        }

        Settings {
            SettingsView()
                .environment(model)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    weak var model: AppModel?

    func applicationDidFinishLaunching(_ notification: Notification) {
        UNUserNotificationCenter.current().delegate = self
    }

    /// Lets the last save finish before quitting, so nothing is lost.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model else { return .terminateNow }
        Task { @MainActor in
            await model.flush()
            sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    // Show alerts even while CardStock is open.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification) async -> UNNotificationPresentationOptions {
        [.banner, .sound]
    }

    // Clicking an alert opens that card.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        guard let link = response.notification.request.content.userInfo["url"] as? String,
              let url = URL(string: link)
        else { return }
        await MainActor.run {
            NSApp.activate()
            NSWorkspace.shared.open(url)
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

        CommandGroup(replacing: .importExport) {
            Button("Export Watchlist…") { model.exportCSV() }
                .keyboardShortcut("e", modifiers: [.command, .shift])
                .disabled(model.items.isEmpty)
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
