import CoreText
import Foundation

/// Mint Display, PriceTag's own heavy price font.
///
/// The font file is embedded in the binary (see `MintDisplayFontData`), so the
/// app works the same from Xcode, `swift run` or a bundled `.app`.
public enum MintDisplay {
    public static let postScriptName = "MintDisplay-Heavy"
    static let unitsPerEm: CGFloat = 1000
    static let capHeight: CGFloat = 740

    /// Every character the font can draw. The formatter only ever produces these.
    public static let supportedCharacters = Set("0123456789$.,-+ ")

    private static let descriptor: CTFontDescriptor? = {
        guard let data = Data(base64Encoded: MintDisplayFontData.base64,
                              options: .ignoreUnknownCharacters) else { return nil }
        return CTFontManagerCreateFontDescriptorFromData(data as CFData)
    }()

    /// True when the embedded font decoded correctly.
    public static var isAvailable: Bool { descriptor != nil }

    /// The font sized so its digits are exactly `pixels` tall.
    public static func font(capHeight pixels: CGFloat) -> CTFont {
        let size = pixels * unitsPerEm / capHeight
        if let descriptor {
            return CTFontCreateWithFontDescriptor(descriptor, size, nil)
        }
        return CTFontCreateWithName("Helvetica-Bold" as CFString, size, nil)
    }
}
