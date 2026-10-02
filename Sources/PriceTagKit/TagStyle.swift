import CoreGraphics

/// The color themes a tag can use.
public enum TagColor: String, Codable, CaseIterable, Identifiable {
    case green, red, gold, white

    public var id: String { rawValue }

    public var name: String {
        switch self {
        case .green: return "Green"
        case .red: return "Red"
        case .gold: return "Gold"
        case .white: return "White"
        }
    }

    public var palette: TagPalette {
        switch self {
        case .green:
            return TagPalette(top: 0xA6FF73, bottom: 0x2BD13B, outline: 0x07280D, depth: 0x0B4316)
        case .red:
            return TagPalette(top: 0xFF8A70, bottom: 0xE8261C, outline: 0x3A0605, depth: 0x6B0F0B)
        case .gold:
            return TagPalette(top: 0xFFEA94, bottom: 0xF5A714, outline: 0x3B2400, depth: 0x6E4300)
        case .white:
            return TagPalette(top: 0xFFFFFF, bottom: 0xD9E1EA, outline: 0x0E1116, depth: 0x2A3039)
        }
    }
}

/// The colors used to paint one tag.
public struct TagPalette: Equatable {
    public var top: UInt32
    public var bottom: UInt32
    public var outline: UInt32
    public var depth: UInt32

    public init(top: UInt32, bottom: UInt32, outline: UInt32, depth: UInt32) {
        self.top = top
        self.bottom = bottom
        self.outline = outline
        self.depth = depth
    }
}

/// Everything about how a tag looks. Effect strengths run from 0 to 1 and are
/// scaled to the text size, so a tag looks the same at every export size.
public struct TagStyle: Codable, Equatable {
    public var color: TagColor = .green
    /// Paint negative prices red no matter which color is picked.
    public var autoRedForNegative = true
    public var outline: Double = 0.55
    public var depth: Double = 0.45
    public var shadow: Double = 0.5
    public var shine = true
    public var slant = false
    /// Extra space between characters, as a fraction of the text height.
    public var spacing: Double = 0.02
    /// Height of the digits in the exported PNG, in pixels.
    public var textHeight: Double = 400

    public init() {}

    public static let textHeights: [Double] = [150, 250, 400, 700]

    public func color(for price: FormattedPrice) -> TagColor {
        autoRedForNegative && price.isNegative ? .red : color
    }
}
