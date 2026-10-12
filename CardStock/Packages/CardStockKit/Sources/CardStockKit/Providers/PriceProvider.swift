import Foundation

/// A source of cards and prices for one game.
public protocol PriceProvider: Sendable {
    var game: Game { get }

    /// Cards matching a search, best matches first. Results may be incomplete
    /// (see `CardQuote.isComplete`).
    func search(_ query: String) async throws -> [CardQuote]

    /// The full details and prices for one card.
    func quote(for sourceID: String) async throws -> CardQuote

    /// Quotes for many cards. Cards that fail are reported in `failures`
    /// rather than failing the whole batch.
    func quotes(for sourceIDs: [String]) async throws -> QuoteBatch
}

public struct QuoteBatch: Sendable {
    /// Keyed by source ID.
    public var quotes: [String: CardQuote]
    /// Source ID → why it failed.
    public var failures: [String: String]

    public init(quotes: [String: CardQuote] = [:], failures: [String: String] = [:]) {
        self.quotes = quotes
        self.failures = failures
    }
}

public enum ProviderError: Error, LocalizedError, Equatable, Sendable {
    case offline
    case rateLimited
    case notFound
    case server(status: Int)
    case badResponse(String)
    case transport(String)

    public var errorDescription: String? {
        switch self {
        case .offline: "You're offline."
        case .rateLimited: "Too many requests. Try again in a minute."
        case .notFound: "Card not found."
        case .server(let status): "The price source had a problem (\(status))."
        case .badResponse: "The price source sent something unexpected."
        case .transport(let message): message
        }
    }
}
