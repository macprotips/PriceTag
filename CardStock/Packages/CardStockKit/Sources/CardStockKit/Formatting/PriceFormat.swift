import Foundation

/// Formats prices and changes the same way everywhere: in the app, the widget
/// and VoiceOver.
public enum PriceFormat {
    /// The minus sign used in changes. A real minus (−) is the width of a
    /// plus, so columns of changes line up.
    static let minus = "\u{2212}"

    /// "$84.20", or "—" when there's no price.
    public static func price(_ amount: Amount?, _ currency: Currency, locale: Locale = .autoupdatingCurrent) -> String {
        guard let amount else { return "—" }
        return amount.decimal.formatted(.currency(code: currency.code).locale(locale))
    }

    /// "+$1.97" or "−$0.30".
    public static func change(_ amount: Amount, _ currency: Currency, locale: Locale = .autoupdatingCurrent) -> String {
        let text = amount.decimal.formatted(.currency(code: currency.code).locale(locale).sign(strategy: .always()))
        return text.replacingOccurrences(of: "-", with: minus)
    }

    /// "+2.4%" or "−1.1%".
    public static func percent(_ fraction: Double, locale: Locale = .autoupdatingCurrent) -> String {
        let text = fraction.formatted(.percent.precision(.fractionLength(1)).sign(strategy: .always()).locale(locale))
        return text.replacingOccurrences(of: "-", with: minus)
    }

    /// The percentage if there is one, otherwise the amount. "—" for no change data.
    public static func change(_ change: PriceChange?, _ currency: Currency, asPercent: Bool = true, locale: Locale = .autoupdatingCurrent) -> String {
        guard let change else { return "—" }
        if asPercent, let fraction = change.fraction {
            return percent(fraction, locale: locale)
        }
        return self.change(change.amount, currency, locale: locale)
    }

    /// For VoiceOver: "up 2.4 percent", "down 30 cents", "unchanged".
    public static func spokenChange(_ change: PriceChange?, _ currency: Currency) -> String {
        guard let change else { return "no change yet" }
        switch change.direction {
        case .flat:
            return "unchanged"
        case .up, .down:
            let word = change.direction == .up ? "up" : "down"
            if let fraction = change.fraction {
                return "\(word) \(abs(fraction).formatted(.percent.precision(.fractionLength(1))))"
            }
            return "\(word) \(price(Amount(cents: abs(change.amount.cents)), currency))"
        }
    }
}
