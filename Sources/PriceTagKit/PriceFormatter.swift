import Foundation

/// How a typed price is turned into the text on the tag.
public struct PriceFormat: Codable, Equatable {
    public enum Cents: String, Codable, CaseIterable, Identifiable {
        /// Show cents only when you type them: "12.5" -> "$12.50", "100" -> "$100".
        case auto
        /// Always show cents: "100" -> "$100.00".
        case always
        /// Round to whole dollars: "12.50" -> "$13".
        case never

        public var id: String { rawValue }
    }

    public var cents: Cents
    public var groupThousands: Bool

    public init(cents: Cents = .auto, groupThousands: Bool = true) {
        self.cents = cents
        self.groupThousands = groupThousands
    }
}

/// The text to draw, plus what the app needs to know about it.
public struct FormattedPrice: Equatable {
    public var text: String
    public var isNegative: Bool

    public init(text: String, isNegative: Bool) {
        self.text = text
        self.isNegative = isNegative
    }
}

public enum PriceFormatter {
    /// The most whole-dollar digits accepted. Anything longer is cut off.
    public static let maxWholeDigits = 10

    /// Turns whatever was typed ("12.5", "$1234", "-110", "(5)", "12,50") into
    /// a clean price like "$12.50". Returns nil when there are no digits.
    public static func format(_ input: String, options: PriceFormat = PriceFormat()) -> FormattedPrice? {
        let trimmed = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.contains(where: \.isASCIIDigit) else { return nil }

        // Sign: the first sign character before any digit wins. "(5)" means negative.
        var negative = false
        var explicitPlus = false
        for ch in trimmed {
            if ch.isASCIIDigit || ch == "." || ch == "," { break }
            if ch == "-" || ch == "\u{2212}" || ch == "\u{2013}" || ch == "(" { negative = true; break }
            if ch == "+" { explicitPlus = true; break }
        }

        let (whole, fraction, hasDecimal) = splitNumber(trimmed)

        let fractionDigits: Int
        switch options.cents {
        case .always: fractionDigits = 2
        case .never: fractionDigits = 0
        case .auto: fractionDigits = hasDecimal && !fraction.isEmpty ? 2 : 0
        }

        let wholeDigits = String(whole.drop(while: { $0 == "0" }).prefix(maxWholeDigits))
        let literal = "\(wholeDigits.isEmpty ? "0" : wholeDigits).\(fraction.isEmpty ? "0" : fraction)"
        guard var value = Decimal(string: literal, locale: Locale(identifier: "en_US_POSIX")) else {
            return nil
        }
        var rounded = Decimal()
        NSDecimalRound(&rounded, &value, fractionDigits, .plain)

        let number = numberString(rounded, fractionDigits: fractionDigits, group: options.groupThousands)
        let isNegative = negative && rounded != 0
        let sign = isNegative ? "-" : (explicitPlus ? "+" : "")
        return FormattedPrice(text: sign + "$" + number, isNegative: isNegative)
    }

    /// Splits typed text into whole and fraction digits, working out whether
    /// "," or "." is the decimal separator.
    static func splitNumber(_ text: String) -> (whole: String, fraction: String, hasDecimal: Bool) {
        let kept = text.filter { $0.isASCIIDigit || $0 == "." || $0 == "," }
        let dots = kept.filter { $0 == "." }.count
        let commas = kept.filter { $0 == "," }.count

        var decimal: Character?
        if dots > 0 && commas > 0 {
            // Whichever comes last is the decimal: "1,234.50" or "1.234,50".
            decimal = kept.last(where: { $0 == "." || $0 == "," })
        } else if dots == 1 {
            decimal = "."
        } else if commas == 1 {
            // "12,5" and "12,50" are decimals; "1,234" is a thousands separator.
            let after = kept.split(separator: ",", omittingEmptySubsequences: false).last ?? ""
            if after.count <= 2 { decimal = "," }
        }

        guard let decimal, let index = kept.lastIndex(of: decimal) else {
            return (kept.filter(\.isASCIIDigit), "", false)
        }
        let whole = kept[..<index].filter(\.isASCIIDigit)
        let fraction = kept[kept.index(after: index)...].filter(\.isASCIIDigit)
        return (String(whole), String(fraction), true)
    }

    static func numberString(_ value: Decimal, fractionDigits: Int, group: Bool) -> String {
        let formatter = NumberFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.numberStyle = .decimal
        formatter.usesGroupingSeparator = group
        formatter.groupingSeparator = ","
        formatter.groupingSize = 3
        formatter.decimalSeparator = "."
        formatter.minimumFractionDigits = fractionDigits
        formatter.maximumFractionDigits = fractionDigits
        formatter.minimumIntegerDigits = 1
        let magnitude = value < 0 ? -value : value
        return formatter.string(from: magnitude as NSDecimalNumber) ?? "\(magnitude)"
    }
}

private extension Character {
    var isASCIIDigit: Bool { ("0"..."9").contains(self) }
}
