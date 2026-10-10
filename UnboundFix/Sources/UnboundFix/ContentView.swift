import SwiftUI

struct ContentView: View {
    @StateObject private var model = FixModel()
    @State private var isTargeted = false

    var body: some View {
        VStack(spacing: 18) {
            header
            content
                .frame(maxWidth: .infinity, minHeight: 230)
        }
        .padding(24)
        .frame(width: 540)
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first, !isBusy else { return false }
            model.load(url)
            return true
        } isTargeted: { isTargeted = $0 }
    }

    private var isBusy: Bool {
        if case .working = model.phase { return true }
        return false
    }

    private var header: some View {
        VStack(spacing: 4) {
            Text("Unbound Black Screen Fix")
                .font(.title2.bold())
            Text("For Need for Speed Unbound, Steam build 16690907, in CrossOver with D3DMetal")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var content: some View {
        switch model.phase {
        case .waiting:
            dropZone

        case .working(let message):
            VStack(spacing: 12) {
                ProgressView()
                Text(message)
            }

        case .ready(let file):
            result(icon: "checkmark.seal.fill", color: .green,
                   title: "Found the right game file",
                   message: "It's the exact version this fix was made for. Make sure the game is closed, then apply the fix. A backup is made first.",
                   file: file) {
                Button("Apply Fix") { model.applyFix(to: file) }
                    .keyboardShortcut(.defaultAction)
            }

        case .alreadyFixed(let file, let canUndo):
            result(icon: "checkmark.circle.fill", color: .green,
                   title: "The fix is already applied",
                   message: "Nothing to do. Launch Unbound through Steam in CrossOver.",
                   file: file) {
                if canUndo {
                    Button("Undo Fix") { model.undoFix(on: file) }
                }
            }

        case .fixed(let file, let backup):
            result(icon: "checkmark.circle.fill", color: .green,
                   title: "Fixed!",
                   message: "Launch Unbound through Steam in CrossOver. To undo, drop the game folder here again. A Steam update or file verification may remove the fix; just run this again afterwards.",
                   file: file) {
                Button("Show Backup") { model.showInFinder(backup) }
            }

        case .undone(let file):
            result(icon: "arrow.uturn.backward.circle.fill", color: .blue,
                   title: "Original file restored",
                   message: "The game is back to how it was before the fix.",
                   file: file) { EmptyView() }

        case .problem(let message, let detail):
            VStack(spacing: 12) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .font(.system(size: 36))
                    .foregroundStyle(.orange)
                Text(message)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                if let detail {
                    Text(detail)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                        .multilineTextAlignment(.center)
                }
                Button("Start Over") { model.startOver() }
                    .keyboardShortcut(.defaultAction)
            }
        }
    }

    private var dropZone: some View {
        VStack(spacing: 12) {
            Image(systemName: "folder.badge.plus")
                .font(.system(size: 44))
                .foregroundStyle(isTargeted ? Color.accentColor : .secondary)
            Text("Drop your Need for Speed Unbound folder here")
                .font(.headline)
            Text("You can also drop your whole CrossOver bottle. The app finds the right file, checks it, backs it up and fixes it.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            Button("Choose Folder…") { model.chooseFolder() }
        }
        .padding(24)
        .frame(maxWidth: .infinity, minHeight: 230)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .strokeBorder(isTargeted ? Color.accentColor : Color.secondary.opacity(0.5),
                              style: StrokeStyle(lineWidth: 2, dash: [8, 6]))
                .background(RoundedRectangle(cornerRadius: 14)
                    .fill(isTargeted ? Color.accentColor.opacity(0.08) : Color.clear))
        )
    }

    private func result<Actions: View>(icon: String, color: Color, title: String, message: String,
                                       file: URL, @ViewBuilder actions: () -> Actions) -> some View {
        VStack(spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 40))
                .foregroundStyle(color)
            Text(title)
                .font(.headline)
            Text(message)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            Text(file.path)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .truncationMode(.middle)
                .textSelection(.enabled)
            HStack {
                Button("Start Over") { model.startOver() }
                actions()
            }
            .padding(.top, 4)
        }
    }
}
