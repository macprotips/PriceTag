import Foundation

/// Spaces requests out so a source never sees more than one per `interval`.
///
/// Each caller reserves the next free slot before sleeping, so concurrent
/// callers queue up instead of all waking at once.
public actor RateLimiter {
    private let interval: Duration
    private var nextSlot: ContinuousClock.Instant

    public init(interval: Duration) {
        self.interval = interval
        nextSlot = .now
    }

    public func wait() async {
        let now = ContinuousClock.now
        let slot = max(now, nextSlot)
        nextSlot = slot + interval
        if slot > now {
            try? await Task.sleep(until: slot, clock: .continuous)
        }
    }
}

/// A small JSON client: sets the headers sources ask for, respects their rate
/// limits and turns failures into `ProviderError`s.
public struct HTTPClient: Sendable {
    let session: URLSession
    let limiter: RateLimiter
    let headers: [String: String]

    public init(session: URLSession = .shared, limiter: RateLimiter, headers: [String: String] = [:]) {
        self.session = session
        self.limiter = limiter
        self.headers = headers
    }

    /// Identifies the app, as Scryfall asks every client to.
    public static var userAgent: String {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.1"
        return "CardStock/\(version) (+https://github.com/macprotips/PriceTag)"
    }

    func get<T: Decodable>(_ url: URL, as type: T.Type, decoder: JSONDecoder = JSONDecoder()) async throws -> T {
        try await send(URLRequest(url: url), as: type, decoder: decoder)
    }

    func post<T: Decodable, Body: Encodable>(_ url: URL, body: Body, as type: T.Type, decoder: JSONDecoder = JSONDecoder()) async throws -> T {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONEncoder().encode(body)
        return try await send(request, as: type, decoder: decoder)
    }

    private func send<T: Decodable>(_ request: URLRequest, as type: T.Type, decoder: JSONDecoder) async throws -> T {
        var request = request
        request.timeoutInterval = 20
        request.setValue(Self.userAgent, forHTTPHeaderField: "User-Agent")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        for (field, value) in headers {
            request.setValue(value, forHTTPHeaderField: field)
        }

        await limiter.wait()

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: request)
        } catch let error as URLError {
            switch error.code {
            case .notConnectedToInternet, .networkConnectionLost, .dataNotAllowed, .internationalRoamingOff:
                throw ProviderError.offline
            case .cancelled:
                throw CancellationError()
            default:
                throw ProviderError.transport(error.localizedDescription)
            }
        }

        if let http = response as? HTTPURLResponse {
            switch http.statusCode {
            case 200..<300: break
            case 404: throw ProviderError.notFound
            case 429: throw ProviderError.rateLimited
            default: throw ProviderError.server(status: http.statusCode)
            }
        }

        do {
            return try decoder.decode(T.self, from: data)
        } catch {
            throw ProviderError.badResponse(String(describing: error))
        }
    }
}
