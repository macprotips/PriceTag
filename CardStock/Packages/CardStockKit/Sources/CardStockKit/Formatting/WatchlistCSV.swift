import Foundation

/// The watchlist as a spreadsheet, for File › Export.
public enum WatchlistCSV {
    public static func make(from items: [WatchItem], currency: Currency, now: Date = Date()) -> String {
        let header = ["Game", "Name", "Set", "Set Code", "Number", "Finish", "Rarity",
                      "Price (\(currency.code))", "Day Change", "Day Change %",
                      "Quantity", "Price Paid", "Value", "Added", "Updated"]
        var lines = [header.map(field).joined(separator: ",")]
        let dateFormat = Date.ISO8601FormatStyle().year().month().day()

        for item in items {
            let price = item.currentAmount(in: currency)
            let change = item.dayChange(in: currency)
            let quantity = item.holding?.quantity
            let paid = item.holding.flatMap { $0.currency == currency ? $0.pricePaid : nil }
            let value = price.flatMap { price in quantity.map { Amount(cents: price.cents * $0) } }
            let row: [String] = [
                item.card.game.name,
                item.card.name,
                item.card.setName,
                item.card.setCode,
                item.card.number,
                item.finish.name,
                item.card.rarity ?? "",
                plain(price),
                plain(change?.amount),
                change?.fraction.map { String(format: "%.2f", $0 * 100) } ?? "",
                quantity.map(String.init) ?? "",
                plain(paid),
                plain(value),
                item.added.formatted(dateFormat),
                item.lastUpdated?.formatted(dateFormat) ?? "",
            ]
            lines.append(row.map(field).joined(separator: ","))
        }
        return lines.joined(separator: "\r\n") + "\r\n"
    }

    /// "12.50", with no currency symbol or grouping, so spreadsheets read it as a number.
    static func plain(_ amount: Amount?) -> String {
        guard let amount else { return "" }
        let sign = amount.cents < 0 ? "-" : ""
        let cents = abs(amount.cents)
        return "\(sign)\(cents / 100).\(String(format: "%02d", cents % 100))"
    }

    /// Quotes a field if it has a comma, quote or line break (RFC 4180).
    static func field(_ value: String) -> String {
        guard value.contains(where: { $0 == "," || $0 == "\"" || $0.isNewline }) else { return value }
        return "\"" + value.replacingOccurrences(of: "\"", with: "\"\"") + "\""
    }
}
