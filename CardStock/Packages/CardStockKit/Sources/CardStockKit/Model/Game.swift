import Foundation

/// A trading card game CardStock can track.
public enum Game: String, Codable, CaseIterable, Identifiable, Sendable {
    case magic
    case pokemon

    public var id: String { rawValue }

    public var name: String {
        switch self {
        case .magic: "Magic"
        case .pokemon: "Pokémon"
        }
    }

    /// Where prices for this game come from.
    public var source: DataSource {
        switch self {
        case .magic: .scryfall
        case .pokemon: .tcgdex
        }
    }
}

/// A price source. Both are free, documented APIs that allow this use.
public enum DataSource: String, Codable, CaseIterable, Identifiable, Sendable {
    case scryfall
    case tcgdex

    public var id: String { rawValue }

    public var name: String {
        switch self {
        case .scryfall: "Scryfall"
        case .tcgdex: "TCGdex"
        }
    }

    public var website: URL {
        switch self {
        case .scryfall: URL(string: "https://scryfall.com")!
        case .tcgdex: URL(string: "https://tcgdex.dev")!
        }
    }

    /// One line for Settings saying what the source provides.
    public var summary: String {
        switch self {
        case .scryfall: "Magic prices from TCGplayer and Cardmarket, updated daily."
        case .tcgdex: "Pokémon prices from TCGplayer and Cardmarket, updated daily."
        }
    }
}

/// The currency prices are shown in. USD prices come from TCGplayer and EUR
/// prices from Cardmarket, for both games.
public enum Currency: String, Codable, CaseIterable, Identifiable, Sendable {
    case usd = "USD"
    case eur = "EUR"

    public var id: String { rawValue }
    public var code: String { rawValue }

    public var name: String {
        switch self {
        case .usd: "US Dollar"
        case .eur: "Euro"
        }
    }

    /// The marketplace prices in this currency come from.
    public var market: String {
        switch self {
        case .usd: "TCGplayer"
        case .eur: "Cardmarket"
        }
    }
}
