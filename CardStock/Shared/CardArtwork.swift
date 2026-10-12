import SwiftUI

/// A card image with the rounded corners of a real card. Shows a plain card
/// shape while loading, and if there's no image.
struct CardArtwork: View {
    var url: URL?
    var width: CGFloat

    /// Real cards are 63 × 88 mm with corners of about 3 mm.
    static let aspectRatio: CGFloat = 63.0 / 88.0
    private var cornerRadius: CGFloat { width * 0.047 }

    var body: some View {
        AsyncImage(url: url, transaction: Transaction(animation: .easeOut(duration: 0.2))) { phase in
            if case .success(let image) = phase {
                image.resizable().aspectRatio(contentMode: .fit)
            } else {
                RoundedRectangle(cornerRadius: cornerRadius, style: .continuous)
                    .fill(.quaternary)
                    .overlay {
                        if url == nil || phase.error != nil {
                            Image(systemName: "rectangle.portrait")
                                .font(.system(size: width * 0.25, weight: .light))
                                .foregroundStyle(.tertiary)
                        }
                    }
            }
        }
        .frame(width: width, height: width / Self.aspectRatio)
        .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        .accessibilityHidden(true)
    }
}
