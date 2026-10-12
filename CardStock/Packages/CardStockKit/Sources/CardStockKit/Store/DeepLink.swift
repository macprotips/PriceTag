import Foundation

/// Links from the widget into the app: `cardstock://card?id=magic:…:foil`.
public enum DeepLink {
    public static let scheme = "cardstock"

    public static func card(_ itemID: String) -> URL {
        var components = URLComponents()
        components.scheme = scheme
        components.host = "card"
        components.queryItems = [URLQueryItem(name: "id", value: itemID)]
        return components.url!
    }

    /// The watchlist item a link points to, if it's a card link.
    public static func itemID(from url: URL) -> String? {
        guard url.scheme == scheme, url.host == "card",
              let components = URLComponents(url: url, resolvingAgainstBaseURL: false)
        else { return nil }
        return components.queryItems?.first { $0.name == "id" }?.value
    }
}
