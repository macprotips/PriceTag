import Foundation

/// Pokémon cards and prices from the TCGdex API (https://tcgdex.dev).
///
/// Free, open source, no key. Each card includes TCGplayer prices (USD, per
/// variant) and Cardmarket prices (EUR, with 1, 7 and 30-day averages).
public struct TCGdexProvider: PriceProvider {
    public let game = Game.pokemon

    private let http: HTTPClient
    private let language: String
    private static let assetsHost = "assets.tcgdex.net"
    /// TCGdex has no batch endpoint, so cards are fetched a few at a time.
    static let concurrentRequests = 4

    public static let sharedClient = HTTPClient(limiter: RateLimiter(interval: .milliseconds(125)))

    public init(http: HTTPClient = TCGdexProvider.sharedClient, language: String = "en") {
        self.http = http
        self.language = language
    }

    private var base: URL { URL(string: "https://api.tcgdex.net/v2/\(language)")! }

    public func search(_ query: String) async throws -> [CardQuote] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return [] }
        var components = URLComponents(url: base.appending(path: "cards"), resolvingAgainstBaseURL: false)!
        components.queryItems = [
            // TCGdex matches names loosely (contains, any case) by default.
            URLQueryItem(name: "name", value: trimmed),
            URLQueryItem(name: "pagination:page", value: "1"),
            URLQueryItem(name: "pagination:itemsPerPage", value: "60"),
        ]
        do {
            let briefs = try await http.get(components.url!, as: [TCGdexCardBrief].self)
            return briefs.map(Self.quote(from:))
        } catch ProviderError.notFound {
            return []
        }
    }

    public func quote(for sourceID: String) async throws -> CardQuote {
        let card = try await http.get(base.appending(path: "cards/\(sourceID)"), as: TCGdexCard.self)
        return Self.quote(from: card)
    }

    public func quotes(for sourceIDs: [String]) async throws -> QuoteBatch {
        var batch = QuoteBatch()
        let unique = Array(Set(sourceIDs)).sorted()
        for start in stride(from: 0, to: unique.count, by: Self.concurrentRequests) {
            let chunk = unique[start..<min(start + Self.concurrentRequests, unique.count)]
            let results = await withTaskGroup(of: (String, Result<CardQuote, Error>).self) { group in
                for id in chunk {
                    group.addTask {
                        do { return (id, .success(try await quote(for: id))) }
                        catch { return (id, .failure(error)) }
                    }
                }
                var results: [(String, Result<CardQuote, Error>)] = []
                for await result in group { results.append(result) }
                return results
            }
            for (id, result) in results {
                switch result {
                case .success(let quote):
                    batch.quotes[id] = quote
                case .failure(let error as ProviderError) where error == .offline:
                    // Everything else will fail the same way.
                    throw error
                case .failure(let error):
                    batch.failures[id] = error.localizedDescription
                }
            }
        }
        return batch
    }

    // MARK: Mapping

    /// A search result has only the name, number and image, so it's marked incomplete.
    static func quote(from brief: TCGdexCardBrief) -> CardQuote {
        let setCode = setID(cardID: brief.id, localID: brief.localId)
        return CardQuote(
            card: Card(
                game: .pokemon,
                sourceID: brief.id,
                name: brief.name,
                setName: setCode.uppercased(),
                setCode: setCode.uppercased(),
                number: brief.localId,
                imageURL: imageURL(brief.image, quality: "high"),
                thumbnailURL: imageURL(brief.image, quality: "low")
            ),
            isComplete: false
        )
    }

    static func quote(from card: TCGdexCard) -> CardQuote {
        let tcgplayer = card.pricing?.tcgplayer
        let cardmarket = card.pricing?.cardmarket
        let finishes = Self.finishes(for: card)

        var prices: [Finish: Price] = [:]
        for finish in finishes {
            let usd = tcgplayer?.variants[finish.rawValue]
            // Cardmarket lists reverse holos separately, in its "-holo" fields.
            let eur: Amount?
            let eur7: Amount?
            let eur30: Amount?
            if finish.isReverse {
                eur = positive(cardmarket?.trendHolo) ?? positive(cardmarket?.avg30Holo)
                eur7 = positive(cardmarket?.avg7Holo)
                eur30 = positive(cardmarket?.avg30Holo)
            } else {
                eur = positive(cardmarket?.trend) ?? positive(cardmarket?.avg30)
                eur7 = positive(cardmarket?.avg7)
                eur30 = positive(cardmarket?.avg30)
            }
            prices[finish] = Price(
                usd: positive(usd?.marketPrice) ?? positive(usd?.midPrice),
                eur: eur,
                usdLow: positive(usd?.lowPrice),
                usdHigh: positive(usd?.highPrice),
                eurAverage7: eur7,
                eurAverage30: eur30
            )
        }

        let setCode = card.set.id.uppercased()
        let search = card.name.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? card.name
        let links = [
            URL(string: "https://www.tcgplayer.com/search/pokemon/product?q=\(search)").map { PurchaseLink(title: "TCGplayer", url: $0) },
            URL(string: "https://www.cardmarket.com/en/Pokemon/Products/Search?searchString=\(search)").map { PurchaseLink(title: "Cardmarket", url: $0) },
        ].compactMap { $0 }

        let updated = [tcgplayer?.updated, cardmarket?.updated].compactMap { $0 }.compactMap(parseDate).max()

        return CardQuote(
            card: Card(
                game: .pokemon,
                sourceID: card.id,
                name: card.name,
                setName: card.set.name,
                setCode: setCode,
                number: card.localId,
                rarity: card.rarity,
                imageURL: imageURL(card.image, quality: "high"),
                thumbnailURL: imageURL(card.image, quality: "low"),
                finishes: finishes,
                links: links
            ),
            prices: prices,
            sourceUpdated: updated
        )
    }

    /// The card's variants, using TCGplayer's names, most common first.
    static func finishes(for card: TCGdexCard) -> [Finish] {
        let order = ["normal", "holofoil", "reverse-holofoil"]
        let priced = card.pricing?.tcgplayer.map { Array($0.variants.keys) } ?? []
        if !priced.isEmpty {
            return priced
                .sorted { (order.firstIndex(of: $0) ?? order.count, $0) < (order.firstIndex(of: $1) ?? order.count, $1) }
                .map(Finish.init(rawValue:))
        }
        var finishes: [Finish] = []
        if card.variants?.normal == true { finishes.append(.normal) }
        if card.variants?.holo == true { finishes.append(.holofoil) }
        if card.variants?.reverse == true { finishes.append(.reverseHolofoil) }
        return finishes.isEmpty ? [.normal] : finishes
    }

    /// `"swsh3-136"` with local ID `"136"` → `"swsh3"`.
    static func setID(cardID: String, localID: String) -> String {
        let suffix = "-\(localID)"
        return cardID.hasSuffix(suffix) ? String(cardID.dropLast(suffix.count)) : cardID
    }

    /// TCGdex gives an image's base URL; the file name picks the size.
    static func imageURL(_ base: String?, quality: String) -> URL? {
        guard let base, !base.isEmpty else { return nil }
        return URL(string: "\(base)/\(quality).png")
    }

    /// Prices of zero mean "no sales", not free.
    static func positive(_ value: Double?) -> Amount? {
        guard let amount = Amount(double: value), amount.isPositive else { return nil }
        return amount
    }

    static func parseDate(_ string: String) -> Date? {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: string) { return date }
        formatter.formatOptions = [.withInternetDateTime]
        return formatter.date(from: string)
    }
}

// MARK: TCGdex JSON

struct TCGdexCardBrief: Decodable {
    let id: String
    let localId: String
    let name: String
    let image: String?
}

struct TCGdexCard: Decodable {
    struct SetBrief: Decodable {
        let id: String
        let name: String
    }

    struct Variants: Decodable {
        let normal: Bool?
        let reverse: Bool?
        let holo: Bool?
        let firstEdition: Bool?
    }

    struct Pricing: Decodable {
        let tcgplayer: TCGplayerPricing?
        let cardmarket: CardmarketPricing?

        init(from decoder: Decoder) throws {
            // A malformed market shouldn't lose the other one.
            let container = try decoder.container(keyedBy: CodingKeys.self)
            tcgplayer = try? container.decodeIfPresent(TCGplayerPricing.self, forKey: .tcgplayer)
            cardmarket = try? container.decodeIfPresent(CardmarketPricing.self, forKey: .cardmarket)
        }

        enum CodingKeys: String, CodingKey {
            case tcgplayer, cardmarket
        }
    }

    let id: String
    let localId: String
    let name: String
    let image: String?
    let rarity: String?
    let set: SetBrief
    let variants: Variants?
    let pricing: Pricing?
}

/// `{ "updated": …, "unit": "USD", "normal": {…}, "reverse-holofoil": {…} }`
struct TCGplayerPricing: Decodable {
    struct Variant: Decodable {
        let lowPrice: Double?
        let midPrice: Double?
        let highPrice: Double?
        let marketPrice: Double?
    }

    let updated: String?
    let variants: [String: Variant]

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: AnyCodingKey.self)
        updated = try? container.decodeIfPresent(String.self, forKey: AnyCodingKey("updated"))
        var variants: [String: Variant] = [:]
        for key in container.allKeys where key.stringValue != "updated" && key.stringValue != "unit" {
            if let variant = try? container.decode(Variant.self, forKey: key) {
                variants[key.stringValue] = variant
            }
        }
        self.variants = variants
    }
}

/// Cardmarket prices in EUR. Fields ending in `-holo` are the reverse holo.
struct CardmarketPricing: Decodable {
    let updated: String?
    let avg: Double?
    let low: Double?
    let trend: Double?
    let avg1: Double?
    let avg7: Double?
    let avg30: Double?
    let trendHolo: Double?
    let avg7Holo: Double?
    let avg30Holo: Double?

    enum CodingKeys: String, CodingKey {
        case updated, avg, low, trend, avg1, avg7, avg30
        case trendHolo = "trend-holo"
        case avg7Holo = "avg7-holo"
        case avg30Holo = "avg30-holo"
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        func number(_ key: CodingKeys) -> Double? { try? container.decodeIfPresent(Double.self, forKey: key) }
        updated = try? container.decodeIfPresent(String.self, forKey: .updated)
        avg = number(.avg)
        low = number(.low)
        trend = number(.trend)
        avg1 = number(.avg1)
        avg7 = number(.avg7)
        avg30 = number(.avg30)
        trendHolo = number(.trendHolo)
        avg7Holo = number(.avg7Holo)
        avg30Holo = number(.avg30Holo)
    }
}

struct AnyCodingKey: CodingKey {
    let stringValue: String
    let intValue: Int?

    init(_ string: String) {
        stringValue = string
        intValue = nil
    }

    init?(stringValue: String) {
        self.init(stringValue)
    }

    init?(intValue: Int) {
        stringValue = String(intValue)
        self.intValue = intValue
    }
}
