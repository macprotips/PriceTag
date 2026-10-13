import Foundation

/// Magic cards and prices from the Scryfall API (https://scryfall.com/docs/api).
///
/// Free, no key. Scryfall asks clients to send a User-Agent and Accept header,
/// keep requests well under 10 a second (2 a second for search and
/// collection), cache what they fetch and not paywall the data. Prices come
/// from TCGplayer (USD) and Cardmarket (EUR) and update once a day.
public struct ScryfallProvider: PriceProvider {
    public let game = Game.magic

    private let http: HTTPClient
    private static let base = URL(string: "https://api.scryfall.com")!
    /// Scryfall's limit for one `/cards/collection` request.
    static let collectionLimit = 75

    /// Shared so every Scryfall request in the process goes through one limiter.
    public static let sharedClient = HTTPClient(limiter: RateLimiter(interval: .milliseconds(500)))

    public init(http: HTTPClient = ScryfallProvider.sharedClient) {
        self.http = http
    }

    public func search(_ query: String) async throws -> [CardQuote] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return [] }
        var components = URLComponents(url: Self.base.appending(path: "cards/search"), resolvingAgainstBaseURL: false)!
        components.queryItems = [
            // Paper printings only: MTGO and Arena-only cards have no market price.
            // Parentheses keep "game:paper" applying to the whole query, even one using "or".
            URLQueryItem(name: "q", value: "(\(trimmed)) game:paper"),
            URLQueryItem(name: "unique", value: "prints"),
            URLQueryItem(name: "order", value: "released"),
            URLQueryItem(name: "dir", value: "desc"),
        ]
        do {
            let list = try await http.get(components.url!, as: ScryfallList.self, decoder: Self.decoder)
            return list.data.prefix(60).map(Self.quote(from:))
        } catch ProviderError.notFound {
            // Scryfall answers 404 when a search has no results…
            return []
        } catch ProviderError.server(status: 400) {
            // …and 400 when it can't understand the query, which to a person is the same thing.
            return []
        }
    }

    public func quote(for sourceID: String) async throws -> CardQuote {
        let card = try await http.get(Self.base.appending(path: "cards/\(sourceID)"), as: ScryfallCard.self, decoder: Self.decoder)
        return Self.quote(from: card)
    }

    public func quotes(for sourceIDs: [String]) async throws -> QuoteBatch {
        var batch = QuoteBatch()
        let unique = Array(Set(sourceIDs)).sorted()
        for start in stride(from: 0, to: unique.count, by: Self.collectionLimit) {
            let chunk = Array(unique[start..<min(start + Self.collectionLimit, unique.count)])
            let body = ScryfallCollectionRequest(identifiers: chunk.map { .init(id: $0) })
            let response: ScryfallCollection
            do {
                response = try await http.post(Self.base.appending(path: "cards/collection"), body: body,
                                               as: ScryfallCollection.self, decoder: Self.decoder)
            } catch ProviderError.offline {
                throw ProviderError.offline
            } catch {
                // Keep the chunks that worked; mark this one's cards as failed.
                for id in chunk { batch.failures[id] = error.localizedDescription }
                continue
            }
            for card in response.data {
                batch.quotes[card.id] = Self.quote(from: card)
            }
            for missing in response.notFound ?? [] {
                if let id = missing.id { batch.failures[id] = "Scryfall no longer lists this card." }
            }
        }
        if batch.quotes.isEmpty, let failure = batch.failures.values.first, !unique.isEmpty {
            // Nothing worked at all: report it as the source failing.
            throw ProviderError.transport(failure)
        }
        return batch
    }

    // MARK: Mapping

    static var decoder: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return decoder
    }

    static func quote(from card: ScryfallCard) -> CardQuote {
        let images = card.imageUris ?? card.cardFaces?.first(where: { $0.imageUris != nil })?.imageUris
        let finishes = (card.finishes ?? ["nonfoil"]).map(Finish.init(rawValue:))

        var prices: [Finish: Price] = [:]
        for finish in finishes {
            let price: Price
            switch finish {
            case .foil:
                price = Price(usd: Amount(string: card.prices.usdFoil ?? ""), eur: Amount(string: card.prices.eurFoil ?? ""))
            case .etched:
                price = Price(usd: Amount(string: card.prices.usdEtched ?? ""), eur: Amount(string: card.prices.eurEtched ?? ""))
            default:
                price = Price(usd: Amount(string: card.prices.usd ?? ""), eur: Amount(string: card.prices.eur ?? ""))
            }
            prices[finish] = price
        }

        var links: [PurchaseLink] = []
        if let url = card.purchaseUris?["tcgplayer"].flatMap(URL.init(string:)) {
            links.append(PurchaseLink(title: "TCGplayer", url: url))
        }
        if let url = card.purchaseUris?["cardmarket"].flatMap(URL.init(string:)) {
            links.append(PurchaseLink(title: "Cardmarket", url: url))
        }

        return CardQuote(
            card: Card(
                game: .magic,
                sourceID: card.id,
                name: card.name,
                setName: card.setName,
                setCode: card.set.uppercased(),
                number: card.collectorNumber,
                rarity: card.rarity.map { $0.prefix(1).uppercased() + $0.dropFirst() },
                imageURL: (images?.large ?? images?.normal).flatMap(URL.init(string:)),
                thumbnailURL: (images?.small ?? images?.normal).flatMap(URL.init(string:)),
                finishes: finishes,
                links: links,
                pageURL: card.scryfallUri.flatMap(URL.init(string:))
            ),
            prices: prices
        )
    }
}

// MARK: Scryfall JSON

struct ScryfallList: Decodable {
    let data: [ScryfallCard]
}

struct ScryfallCollectionRequest: Encodable {
    struct Identifier: Encodable {
        let id: String
    }

    let identifiers: [Identifier]
}

struct ScryfallCollection: Decodable {
    struct Identifier: Decodable {
        let id: String?
    }

    let data: [ScryfallCard]
    let notFound: [Identifier]?
}

struct ScryfallCard: Decodable {
    struct ImageURIs: Decodable {
        let small: String?
        let normal: String?
        let large: String?
    }

    struct Face: Decodable {
        let imageUris: ImageURIs?
    }

    struct Prices: Decodable {
        let usd: String?
        let usdFoil: String?
        let usdEtched: String?
        let eur: String?
        let eurFoil: String?
        let eurEtched: String?
    }

    let id: String
    let name: String
    let set: String
    let setName: String
    let collectorNumber: String
    let rarity: String?
    let imageUris: ImageURIs?
    let cardFaces: [Face]?
    let prices: Prices
    let finishes: [String]?
    let purchaseUris: [String: String]?
    let scryfallUri: String?
}
