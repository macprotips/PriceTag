import PriceTagKit
import SwiftUI

/// Shows the tag on a chosen background. Drag it straight into Final Cut,
/// Premiere, DaVinci, CapCut or Finder.
struct PreviewCanvas: View {
    @EnvironmentObject private var model: AppModel
    @State private var hovering = false

    var body: some View {
        ZStack {
            background
            if let image = model.preview {
                Image(decorative: image, scale: 2)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: CGFloat(image.width) / 2, maxHeight: CGFloat(image.height) / 2)
                    .padding(28)
                    .onDrag { model.dragItem() }
                    .help("Drag into your video editor or Finder")
            } else {
                emptyState
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .clipShape(RoundedRectangle(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).strokeBorder(Color.primary.opacity(0.1)))
        .overlay(alignment: .topTrailing) { backgroundPicker.padding(10) }
        .overlay(alignment: .bottomLeading) { info.padding(12) }
        .onHover { hovering = $0 }
    }

    @ViewBuilder
    private var background: some View {
        switch model.previewBackground {
        case .checker:
            Checkerboard()
        case .dark:
            Color(white: 0.11)
        case .light:
            Color(white: 0.95)
        case .video:
            // A stand-in for a felt playmat / card table shot.
            LinearGradient(colors: [Color(red: 0.16, green: 0.26, blue: 0.38),
                                    Color(red: 0.05, green: 0.09, blue: 0.16)],
                           startPoint: .top, endPoint: .bottom)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 10) {
            Image(systemName: "tag")
                .font(.system(size: 40, weight: .light))
            Text("Type a price above")
                .font(.title3.weight(.semibold))
            Text("12.5  →  $12.50     -110  →  -$110     .71  →  $0.71")
                .font(.callout.monospaced())
        }
        .foregroundStyle(model.previewBackground == .light || model.previewBackground == .checker
                         ? Color.black.opacity(0.45) : Color.white.opacity(0.6))
    }

    private var backgroundPicker: some View {
        Picker("Background", selection: $model.previewBackground) {
            ForEach(PreviewBackground.allCases) { bg in
                Text(bg.label).tag(bg)
            }
        }
        .labelsHidden()
        .pickerStyle(.menu)
        .fixedSize()
        .help("Preview background (the PNG is always transparent)")
    }

    @ViewBuilder
    private var info: some View {
        if let size = model.exportSize {
            HStack(spacing: 6) {
                Image(systemName: "hand.draw")
                Text("Drag to export  ·  \(Int(size.width)) × \(Int(size.height)) px PNG")
            }
            .font(.caption.weight(.medium))
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(.thinMaterial, in: Capsule())
            .opacity(hovering ? 1 : 0.75)
        }
    }
}

struct Checkerboard: View {
    var body: some View {
        Canvas { context, size in
            let tile: CGFloat = 12
            context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(white: 0.97)))
            var path = Path()
            for row in 0...Int(size.height / tile) {
                for col in 0...Int(size.width / tile) where (row + col) % 2 == 0 {
                    path.addRect(CGRect(x: CGFloat(col) * tile, y: CGFloat(row) * tile, width: tile, height: tile))
                }
            }
            context.fill(path, with: .color(Color(white: 0.87)))
        }
    }
}
