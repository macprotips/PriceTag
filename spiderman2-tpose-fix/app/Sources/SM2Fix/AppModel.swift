import AppKit
import Foundation
import SM2FixKit

/// A message shown under the game card.
struct Banner: Equatable {
    enum Kind { case success, warning, error }
    var kind: Kind
    var title: String
    var detail: String = ""
}

/// The app's state. All the work happens in SM2FixKit, off the main thread.
final class AppModel: ObservableObject {
    static let shared = AppModel()

    @Published var game: GameInstall?
    @Published var bottleID: String?
    @Published var status: FixStatus?
    @Published var busy = false
    @Published var busyText = ""
    @Published var banner: Banner?
    /// More than one copy of the game was found: the user picks one.
    @Published var choices: [GameInstall] = []

    let env = SystemEnvironment()
    let payload: Result<Payload, Error>
    private let queue = DispatchQueue(label: "sm2fix.work", qos: .userInitiated)
    private var searchedAtLaunch = false

    init() {
        payload = Result { try AppModel.loadPayload() }
        if case .failure(let error) = payload {
            banner = Banner(kind: .error, title: "This copy of the app is incomplete.", detail: error.localizedDescription)
        }
    }

    static func loadPayload() throws -> Payload {
        let resources = Bundle.main.resourceURL ?? Bundle.main.bundleURL
        return try Payload(directory: resources.appendingPathComponent("payload"))
    }

    var selectedBottle: Bottle? {
        game?.candidates.first { $0.id == bottleID } ?? game?.bottle
    }

    var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
    }

    // MARK: - Finding the game

    /// Looks for the game once at launch, and picks it if there's exactly one copy.
    func searchAtLaunch() {
        guard !searchedAtLaunch, game == nil else { return }
        searchedAtLaunch = true
        findAutomatically(quiet: true)
    }

    func findAutomatically(quiet: Bool = false) {
        busy = true
        busyText = "Looking for Spider-Man 2…"
        let env = self.env
        queue.async {
            let found = GameFinder.findAll(env: env)
            DispatchQueue.main.async {
                self.busy = false
                if found.count == 1 {
                    self.select(found[0])
                } else if found.count > 1 {
                    self.choices = found
                } else if !quiet {
                    self.banner = Banner(kind: .warning, title: "Couldn't find Spider-Man 2 automatically.",
                                         detail: "Drag the game's folder (Marvel's Spider-Man 2) into this window, or click Choose Folder.")
                }
            }
        }
    }

    /// Something dropped on the window or the app icon, or picked in the open panel.
    func open(_ url: URL) {
        busy = true
        busyText = "Checking \(url.lastPathComponent)…"
        banner = nil
        let env = self.env
        queue.async {
            let folder = GameFinder.gameFolder(from: url)
            let install = folder.map { GameFinder.install(at: $0, env: env) }
            DispatchQueue.main.async {
                self.busy = false
                if let install {
                    self.select(install)
                } else {
                    self.banner = Banner(kind: .error, title: "That's not the Spider-Man 2 folder.",
                                         detail: "Drop the folder that contains Spider-Man2.exe. In Steam: right-click the game > Manage > Browse local files.")
                }
            }
        }
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Choose the Marvel's Spider-Man 2 folder"
        panel.message = "Choose the folder that contains Spider-Man2.exe (or the exe itself)."
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        if panel.runModal() == .OK, let url = panel.url {
            open(url)
        }
    }

    func select(_ install: GameInstall) {
        choices = []
        game = install
        bottleID = install.bottle?.id
        refreshStatus()
    }

    func forget() {
        game = nil
        status = nil
        banner = nil
        choices = []
    }

    func refreshStatus() {
        guard let game, case .success(let payload) = payload else { return }
        status = Installer(payload: payload, env: env).status(of: game.folder)
    }

    // MARK: - Installing

    func apply() { run(remove: false) }
    func remove() { run(remove: true) }

    private func run(remove: Bool) {
        guard var game, case .success(let payload) = payload else { return }
        game.bottle = selectedBottle
        busy = true
        busyText = remove ? "Removing the fix…" : "Applying the fix…"
        banner = nil
        let installer = Installer(payload: payload, env: env)
        queue.async {
            let result = Result { try remove ? installer.uninstall(game) : installer.install(game) }
            DispatchQueue.main.async {
                self.busy = false
                self.refreshStatus()
                self.banner = Self.banner(for: result, removing: remove, bottle: game.bottle)
            }
        }
    }

    static func banner(for result: Result<InstallReport, Error>, removing: Bool, bottle: Bottle?) -> Banner {
        let report: InstallReport
        switch result {
        case .failure(let error):
            return Banner(kind: .error, title: removing ? "Couldn't remove the fix." : "Couldn't apply the fix.",
                          detail: error.localizedDescription)
        case .success(let r):
            report = r
        }
        var notes = report.notes
        let manual = "In CrossOver: select the bottle > Wine Configuration > Libraries > type d3d12 > Add > Edit > “Native then Builtin”."
        if removing {
            return Banner(kind: .success, title: "The fix is removed. The game is back to normal.",
                          detail: notes.joined(separator: "\n"))
        }
        switch report.override {
        case .wineRunning?:
            notes.insert("One more step: quit CrossOver completely (CrossOver menu > Quit CrossOver), then click Apply Fix again. "
                         + "Wine has to be told once to load the fix, and it can't be while it's running.", at: 0)
            return Banner(kind: .warning, title: "Almost done.", detail: notes.joined(separator: "\n\n"))
        case .failed(let reason)?:
            notes.insert("Wine couldn't be told to load the fix (\(reason)). Do it once by hand: \(manual)", at: 0)
            return Banner(kind: .warning, title: "Fix copied, one manual step left.", detail: notes.joined(separator: "\n\n"))
        case nil:
            notes.insert("No bottle was selected, so if the fix doesn't take effect, do this once: \(manual)", at: 0)
            return Banner(kind: .warning, title: "Fix copied.", detail: notes.joined(separator: "\n\n"))
        default:
            notes.insert("Start Spider-Man 2 from \(bottle?.kind.rawValue ?? "CrossOver") as usual. "
                         + "A log is written to sm2fix.log in the game folder.", at: 0)
            return Banner(kind: .success, title: "The fix is applied.", detail: notes.joined(separator: "\n\n"))
        }
    }

    func showInFinder() {
        guard let game else { return }
        NSWorkspace.shared.activateFileViewerSelecting([game.folder.appendingPathComponent(GameFinder.exeName)])
    }
}
