import Foundation

/// An amount of money in whole cents (or euro cents).
///
/// Prices are stored as integers so they never pick up floating point error:
/// $0.10 stays $0.10 through every save and load. Use `double` only for
/// drawing charts and working out percentages.
public struct Amount: Hashable, Comparable, Sendable, Codable {
    public var cents: Int

    public init(cents: Int) {
        self.cents = cents
    }

    /// Parses a plain decimal string such as `"12.5"` or `"0.07"`.
    public init?(string: String) {
        let trimmed = string.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty,
              let decimal = Decimal(string: trimmed, locale: Locale(identifier: "en_US_POSIX"))
        else { return nil }
        self.init(decimal: decimal)
    }

    /// Rounds to the nearest cent.
    public init(decimal: Decimal) {
        var value = decimal * 100
        var rounded = Decimal()
        NSDecimalRound(&rounded, &value, 0, .plain)
        cents = NSDecimalNumber(decimal: rounded).intValue
    }

    /// Rounds to the nearest cent. Returns nil for nil, NaN or infinity.
    public init?(double: Double?) {
        guard let double, double.isFinite else { return nil }
        cents = Int((double * 100).rounded())
    }

    public static let zero = Amount(cents: 0)

    public var decimal: Decimal { Decimal(cents) / 100 }
    public var double: Double { Double(cents) / 100 }
    public var isZero: Bool { cents == 0 }
    public var isPositive: Bool { cents > 0 }

    public static func < (lhs: Amount, rhs: Amount) -> Bool { lhs.cents < rhs.cents }
    public static func + (lhs: Amount, rhs: Amount) -> Amount { Amount(cents: lhs.cents + rhs.cents) }
    public static func - (lhs: Amount, rhs: Amount) -> Amount { Amount(cents: lhs.cents - rhs.cents) }

    public init(from decoder: Decoder) throws {
        cents = try decoder.singleValueContainer().decode(Int.self)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(cents)
    }
}

/// How much a price moved between two points.
public struct PriceChange: Hashable, Sendable, Codable {
    public enum Direction: Sendable {
        case up, down, flat
    }

    public var amount: Amount
    /// The change as a fraction of the starting price (0.024 is 2.4%).
    /// Nil when the starting price was zero.
    public var fraction: Double?

    public init(amount: Amount, fraction: Double?) {
        self.amount = amount
        self.fraction = fraction
    }

    public init(from start: Amount, to end: Amount) {
        amount = end - start
        fraction = start.isZero ? nil : Double(end.cents - start.cents) / Double(start.cents)
    }

    public var direction: Direction {
        if amount.cents > 0 { return .up }
        if amount.cents < 0 { return .down }
        return .flat
    }
}
