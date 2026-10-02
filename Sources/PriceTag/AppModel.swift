import AppKit
import PriceTagKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class AppModel: ObservableObject {
    @Published var input = "" { didSet { refresh() } }
    @Published var style: TagStyle { didSet { persist(); refresh() } }
    @Published var format: PriceFormat { didSet { persist(); refresh() } }
    @Published var exportFolder: URL { didSet { persist() } }
    @Published var previewBackground: PreviewBackground = .checker
    @Published var showingBatch = false

    @Published private(set) var price: FormattedPrice?
    @Published private(set) var preview: CGImage?
    @Published private(set) var toast: Toast?

    /// Pixel size the preview is rendered at. The look scales with text size,
    /// so this matches the export exactly, just smaller.
    private let previewTextHeight: CGFloat = 260
    private var toastTask: Task<Void, Never>?
    private let defaults = UserDefaults.standard

    init() {
        let decoder = JSONDecoder()
        style = defaults.data(forKey: Keys.style).flatMap { try? decoder.decode(TagStyle.self, from: $0) } ?? TagStyle()
        format = defaults.data(forKey: Keys.format).flatMap { try? decoder.decode(PriceFormat.self, from: $0) } ?? PriceFormat()
        if let path = defaults.string(forKey: Keys.folder) {
            exportFolder = URL(fileURLWithPath: path, isDirectory: true)
        } else {
            exportFolder = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("PriceTag", isDirectory: true)
        }
        refresh()
    }

    // MARK: State

    var color: TagColor? { price.map { style.color(for: $0) } }

    /// Pixel size of the PNG that will be exported.
    var exportSize: CGSize? {
        guard let preview else { return nil }
        let scale = CGFloat(style.textHeight) / previewTextHeight
        return CGSize(width: (CGFloat(preview.width) * scale).rounded(),
                      height: (CGFloat(preview.height) * scale).rounded())
    }

    func toggleGreenRed() {
        style.color = style.color == .green ? .red : .green
    }

    func resetStyle() {
        let height = style.textHeight
        style = TagStyle()
        style.textHeight = height
    }

    private func refresh() {
        price = PriceFormatter.format(input, options: format)
        if let price {
            preview = TagRenderer.render(price.text, color: style.color(for: price), style: style,
                                         textHeight: previewTextHeight)
        } else {
            preview = nil
        }
    }

    private func persist() {
        let encoder = JSONEncoder()
        defaults.set(try? encoder.encode(style), forKey: Keys.style)
        defaults.set(try? encoder.encode(format), forKey: Keys.format)
        defaults.set(exportFolder.path, forKey: Keys.folder)
    }

    // MARK: Export

    /// Full-size PNG for a price, or for the current one.
    func pngData(for price: FormattedPrice? = nil) -> Data? {
        guard let price = price ?? self.price,
              let image = TagRenderer.render(price.text, color: style.color(for: price), style: style)
        else { return nil }
        return TagRenderer.pngData(image)
    }

    func fileName(for price: FormattedPrice) -> String {
        "\(price.text) \(style.color(for: price).name.lowercased()).png"
    }

    func copyToClipboard() {
        guard let png = pngData() else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        let item = NSPasteboardItem()
        item.setData(png, forType: .png)
        if let tiff = NSBitmapImageRep(data: png)?.tiffRepresentation {
            item.setData(tiff, forType: .tiff)
        }
        pasteboard.writeObjects([item])
        show(Toast("Copied \(price?.text ?? "") to the clipboard", symbol: "doc.on.clipboard"))
    }

    /// Writes the current tag into the export folder and returns its URL.
    @discardableResult
    func saveToExportFolder(announce: Bool = true) -> URL? {
        guard let price, let png = pngData(for: price) else { return nil }
        do {
            try FileManager.default.createDirectory(at: exportFolder, withIntermediateDirectories: true)
            let url = exportFolder.appendingPathComponent(fileName(for: price))
            try png.write(to: url, options: .atomic)
            if announce {
                show(Toast("Saved \(url.lastPathComponent)", symbol: "checkmark.circle.fill",
                           action: { NSWorkspace.shared.activateFileViewerSelecting([url]) }))
            }
            return url
        } catch {
            show(Toast("Couldn't save: \(error.localizedDescription)", symbol: "exclamationmark.triangle.fill"))
            return nil
        }
    }

    func saveAs() {
        guard let price, let png = pngData(for: price) else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = fileName(for: price)
        panel.directoryURL = exportFolder
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try png.write(to: url, options: .atomic)
            show(Toast("Saved \(url.lastPathComponent)", symbol: "checkmark.circle.fill",
                       action: { NSWorkspace.shared.activateFileViewerSelecting([url]) }))
        } catch {
            show(Toast("Couldn't save: \(error.localizedDescription)", symbol: "exclamationmark.triangle.fill"))
        }
    }

    /// Drag-out source. The file goes to the export folder (not a temp folder)
    /// so editors that link to media, like Premiere, never lose it.
    func dragItem() -> NSItemProvider {
        guard let url = saveToExportFolder(announce: false),
              let provider = NSItemProvider(contentsOf: url) else { return NSItemProvider() }
        provider.suggestedName = url.deletingPathExtension().lastPathComponent
        return provider
    }

    func chooseExportFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Use Folder"
        panel.message = "PNGs you save, drag or batch export go here."
        panel.directoryURL = exportFolder
        if panel.runModal() == .OK, let url = panel.url {
            exportFolder = url
        }
    }

    func revealExportFolder() {
        try? FileManager.default.createDirectory(at: exportFolder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(exportFolder)
    }

    // MARK: Batch

    func batchPrices(from text: String) -> [FormattedPrice] {
        var seen = Set<String>()
        return text.split(whereSeparator: \.isNewline)
            .compactMap { PriceFormatter.format(String($0), options: format) }
            .filter { seen.insert($0.text).inserted }
    }

    /// Exports every price and returns the files written.
    func exportBatch(_ prices: [FormattedPrice]) -> [URL] {
        do {
            try FileManager.default.createDirectory(at: exportFolder, withIntermediateDirectories: true)
        } catch {
            show(Toast("Couldn't create the export folder", symbol: "exclamationmark.triangle.fill"))
            return []
        }
        var written: [URL] = []
        for price in prices {
            guard let png = pngData(for: price) else { continue }
            let url = exportFolder.appendingPathComponent(fileName(for: price))
            if (try? png.write(to: url, options: .atomic)) != nil {
                written.append(url)
            }
        }
        if !written.isEmpty {
            show(Toast("Exported \(written.count) price\(written.count == 1 ? "" : "s")",
                       symbol: "checkmark.circle.fill",
                       action: { NSWorkspace.shared.activateFileViewerSelecting(written) }))
        }
        return written
    }

    // MARK: Toast

    func show(_ toast: Toast) {
        toastTask?.cancel()
        withAnimation(.spring(response: 0.3, dampingFraction: 0.85)) { self.toast = toast }
        toastTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 2_600_000_000)
            guard !Task.isCancelled else { return }
            withAnimation(.easeOut(duration: 0.25)) { self?.toast = nil }
        }
    }

    private enum Keys {
        static let style = "tagStyle"
        static let format = "priceFormat"
        static let folder = "exportFolder"
    }
}

struct Toast: Identifiable {
    let id = UUID()
    let message: String
    let symbol: String
    let action: (() -> Void)?

    init(_ message: String, symbol: String, action: (() -> Void)? = nil) {
        self.message = message
        self.symbol = symbol
        self.action = action
    }
}

enum PreviewBackground: String, CaseIterable, Identifiable {
    case checker, dark, light, video

    var id: String { rawValue }

    var label: String {
        switch self {
        case .checker: return "Transparent"
        case .dark: return "Dark"
        case .light: return "Light"
        case .video: return "Card table"
        }
    }
}
