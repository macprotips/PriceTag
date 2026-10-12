import Foundation

/// The folder and settings the app and the widget share.
///
/// The App Group ID is read from the `CardStockAppGroup` key in Info.plist,
/// which Xcode fills in with the team ID. Without a team (an unsigned build)
/// there's no shared container, so files go to Application Support instead;
/// the app works, but the widget can't see its data.
public enum SharedContainer {
    public static var appGroupID: String? {
        guard let id = Bundle.main.object(forInfoDictionaryKey: "CardStockAppGroup") as? String,
              !id.isEmpty, !id.contains("$(")
        else { return nil }
        return id
    }

    public static var directory: URL {
        if let appGroupID,
           let url = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroupID) {
            return url
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CardStock", isDirectory: true)
        try? FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        return support
    }

    public static var watchlistURL: URL { directory.appendingPathComponent("watchlist.json") }
    public static var snapshotURL: URL { directory.appendingPathComponent("widget-snapshot.json") }
    public static var thumbnailsURL: URL { directory.appendingPathComponent("Thumbnails", isDirectory: true) }

    public static var defaults: UserDefaults {
        appGroupID.flatMap(UserDefaults.init(suiteName:)) ?? .standard
    }
}

/// Settings the widget needs to see too.
public enum Preferences {
    private static let currencyKey = "currency"

    public static var currency: Currency {
        get { SharedContainer.defaults.string(forKey: currencyKey).flatMap(Currency.init(rawValue:)) ?? defaultCurrency }
        set { SharedContainer.defaults.set(newValue.rawValue, forKey: currencyKey) }
    }

    /// Euros in the eurozone, dollars everywhere else.
    static var defaultCurrency: Currency {
        Locale.current.currency?.identifier == "EUR" ? .eur : .usd
    }
}
