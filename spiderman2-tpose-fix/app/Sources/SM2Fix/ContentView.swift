import AppKit
import SwiftUI
import SM2FixKit

struct ContentView: View {
    @ObservedObject var model: AppModel
    @State private var dropTargeted = false
    @State private var showDetails = false

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            header
            if let game = model.game {
                gameCard(game)
            } else if !model.choices.isEmpty {
                choiceList
            } else {
                dropZone
            }
            if model.busy {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(model.busyText).foregroundStyle(.secondary)
                }
            }
            if let banner = model.banner {
                BannerView(banner: banner)
            }
            Spacer(minLength: 0)
            footer
        }
        .padding(24)
        .frame(width: 540)
        .frame(minHeight: 560)
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            model.open(url)
            return true
        } isTargeted: { dropTargeted = $0 }
        .onAppear { model.searchAtLaunch() }
        .disabled(model.busy)
    }

    // MARK: - Pieces

    private var header: some View {
        HStack(spacing: 14) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 64, height: 64)
            VStack(alignment: .leading, spacing: 4) {
                Text("Spider-Man 2 T-Pose Fix")
                    .font(.title2.weight(.semibold))
                Text("Fixes characters stuck in a T-pose in Marvel's Spider-Man 2 on M1, M2, M3 and M4 Macs (CrossOver or Whisky).")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var dropZone: some View {
        VStack(spacing: 14) {
            Image(systemName: "folder.badge.plus")
                .font(.system(size: 40, weight: .light))
                .foregroundStyle(dropTargeted ? Color.accentColor : .secondary)
            Text("Drag your Marvel's Spider-Man 2 folder here")
                .font(.headline)
            Text("The folder with Spider-Man2.exe in it. In Steam: right-click the game > Manage > Browse local files.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 10) {
                Button("Choose Folder…") { model.chooseFolder() }
                Button("Find It for Me") { model.findAutomatically() }
            }
            .padding(.top, 4)
        }
        .padding(24)
        .frame(maxWidth: .infinity, minHeight: 230)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(dropTargeted ? Color.accentColor.opacity(0.10) : Color.primary.opacity(0.03))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [7, 5]))
                .foregroundStyle(dropTargeted ? Color.accentColor : Color.secondary.opacity(0.5))
        )
        .animation(.easeOut(duration: 0.15), value: dropTargeted)
    }

    private var choiceList: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Found Spider-Man 2 in more than one place. Which one do you play?")
                .font(.headline)
            ForEach(model.choices, id: \.folder) { install in
                Button {
                    model.select(install)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(install.folder.abbreviatedPath).lineLimit(1).truncationMode(.middle)
                        Text(install.bottle?.displayName ?? "bottle unknown").font(.caption).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            Button("Choose a Different Folder…") { model.chooseFolder() }
                .buttonStyle(.link)
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.primary.opacity(0.04)))
    }

    private func gameCard(_ game: GameInstall) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            row("Game") {
                HStack {
                    Text(game.folder.abbreviatedPath)
                        .lineLimit(2)
                        .truncationMode(.middle)
                        .textSelection(.enabled)
                    Spacer()
                    Button("Change…") { model.forget() }
                        .buttonStyle(.link)
                }
            }
            row("Bottle") {
                if game.candidates.count > 1 {
                    Picker("", selection: $model.bottleID) {
                        if game.bottle == nil {
                            Text("Choose the bottle you play in…").tag(String?.none)
                        }
                        ForEach(game.candidates) { bottle in
                            Text(bottle.displayName).tag(Optional(bottle.id))
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                } else if let bottle = game.bottle {
                    Text(bottle.displayName)
                } else {
                    Text("No CrossOver or Whisky bottle found").foregroundStyle(.secondary)
                }
            }
            row("Status") { StatusBadge(status: model.status) }

            HStack(spacing: 10) {
                Button(action: model.apply) {
                    Text(applyTitle).frame(minWidth: 120)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .keyboardShortcut(.defaultAction)
                .disabled(game.candidates.count > 1 && model.bottleID == nil)

                if model.status != nil && model.status != .notInstalled {
                    Button("Remove Fix", action: model.remove)
                        .controlSize(.large)
                }
                Spacer()
                Button {
                    model.showInFinder()
                } label: {
                    Image(systemName: "magnifyingglass")
                }
                .help("Show the game folder in Finder")
            }
            .padding(.top, 2)
        }
        .padding(18)
        .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.primary.opacity(0.04)))
    }

    private var applyTitle: String {
        switch model.status {
        case .installed?: return "Reinstall Fix"
        case .updateAvailable?: return "Update Fix"
        case .testBuildInstalled?: return "Replace Test with Fix"
        default: return "Apply Fix"
        }
    }

    private func row<Content: View>(_ label: String, @ViewBuilder content: () -> Content) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(width: 52, alignment: .leading)
            content()
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            DisclosureGroup("What does it do?", isExpanded: $showDetails) {
                Text("""
                On M1–M4 Macs, the GPU returns zeros when a shader reads a “typed” buffer past element 268,435,456. \
                The game keeps all character meshes in one 2 GB buffer and reads it exactly that way, so characters \
                stored in its upper half never get animated. The fix adds a small d3d12.dll to the game folder that \
                makes the game read that buffer the “raw” way (with two rewritten shaders), and tells Wine to load it. \
                Nothing else in the game or your bottle is changed, and Remove Fix undoes it.
                """)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 4)
            }
            HStack {
                Link("Source and details",
                     destination: URL(string: "https://github.com/macprotips/PriceTag/tree/claude/elegant-dijkstra-9yhhvx/spiderman2-tpose-fix")!)
                Spacer()
                Text("Version \(model.version) · Not affiliated with Sony, Insomniac or Marvel")
                    .foregroundStyle(.tertiary)
            }
            .font(.caption)
        }
    }
}

struct StatusBadge: View {
    let status: FixStatus?

    var body: some View {
        let (text, color, symbol): (String, Color, String) = {
            switch status {
            case .installed?: return ("Fix installed", .green, "checkmark.circle.fill")
            case .updateAvailable?: return ("Older version of the fix installed", .orange, "arrow.triangle.2.circlepath.circle.fill")
            case .testBuildInstalled?: return ("A troubleshooting test is installed", .orange, "wrench.and.screwdriver.fill")
            case .notInstalled?: return ("Not installed", .secondary, "circle.dashed")
            case nil: return ("Checking…", .secondary, "circle.dashed")
            }
        }()
        Label(text, systemImage: symbol)
            .foregroundStyle(color)
    }
}

struct BannerView: View {
    let banner: Banner

    var body: some View {
        let (color, symbol): (Color, String) = {
            switch banner.kind {
            case .success: return (.green, "checkmark.seal.fill")
            case .warning: return (.orange, "exclamationmark.triangle.fill")
            case .error: return (.red, "xmark.octagon.fill")
            }
        }()
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: symbol)
                .foregroundStyle(color)
                .font(.title3)
            VStack(alignment: .leading, spacing: 4) {
                Text(banner.title).font(.headline)
                if !banner.detail.isEmpty {
                    Text(banner.detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(color.opacity(0.12)))
    }
}

extension URL {
    /// The path with the home folder shown as ~.
    var abbreviatedPath: String {
        (path as NSString).abbreviatingWithTildeInPath
    }
}
