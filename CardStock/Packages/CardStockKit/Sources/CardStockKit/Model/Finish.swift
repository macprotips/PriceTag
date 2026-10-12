import Foundation

/// A card's finish or print variant: non-foil, foil, holo, reverse holo…
///
/// It's a string rather than an enum because Pokémon variants come straight
/// from the source (`"1st-edition-holofoil"` and the like), and new ones turn
/// up with new sets.
public struct Finish: RawRepresentable, Hashable, Codable, Identifiable, Sendable {
    public let rawValue: String

    public init(rawValue: String) {
        self.rawValue = rawValue
    }

    public init(_ rawValue: String) {
        self.rawValue = rawValue
    }

    public var id: String { rawValue }

    // Magic, as Scryfall names them.
    public static let nonfoil = Finish("nonfoil")
    public static let foil = Finish("foil")
    public static let etched = Finish("etched")

    // Pokémon, as TCGplayer names them in TCGdex.
    public static let normal = Finish("normal")
    public static let holofoil = Finish("holofoil")
    public static let reverseHolofoil = Finish("reverse-holofoil")

    /// Whether this is a reverse holo, which Cardmarket prices separately.
    public var isReverse: Bool { rawValue.lowercased().contains("reverse") }

    /// The name to show, like "Foil" or "Reverse Holo".
    public var name: String {
        switch self {
        case .nonfoil, .normal: return "Normal"
        case .foil: return "Foil"
        case .etched: return "Etched Foil"
        case .holofoil: return "Holo"
        case .reverseHolofoil: return "Reverse Holo"
        default: return Self.humanize(rawValue)
        }
    }

    /// `"1st-edition-holofoil"` or `"1stEditionHolofoil"` → `"1st Edition Holo"`.
    static func humanize(_ raw: String) -> String {
        var spaced = ""
        for character in raw {
            if character == "-" || character == "_" {
                spaced.append(" ")
            } else if character.isUppercase, let last = spaced.last, last != " " {
                spaced.append(" ")
                spaced.append(character)
            } else {
                spaced.append(character)
            }
        }
        return spaced
            .split(separator: " ")
            .map { word -> String in
                let lower = word.lowercased()
                if lower == "holofoil" { return "Holo" }
                return lower.prefix(1).uppercased() + lower.dropFirst()
            }
            .joined(separator: " ")
    }
}
