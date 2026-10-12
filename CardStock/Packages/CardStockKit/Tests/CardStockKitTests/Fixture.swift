@testable import CardStockKit
import Foundation
import XCTest

enum Fixture {
    static func data(_ name: String) throws -> Data {
        let url = try XCTUnwrap(Bundle.module.url(forResource: name, withExtension: "json", subdirectory: "Fixtures"),
                                "Missing fixture \(name).json")
        return try Data(contentsOf: url)
    }
}

/// A Gregorian calendar in UTC, so day boundaries don't depend on where tests run.
let utc: Calendar = {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = TimeZone(identifier: "UTC")!
    return calendar
}()

/// Noon UTC on a given day of October 2026.
func day(_ day: Int, month: Int = 10, hour: Int = 12) -> Date {
    utc.date(from: DateComponents(year: 2026, month: month, day: day, hour: hour))!
}

func sampleCard(_ id: String = "abc", game: Game = .magic) -> Card {
    Card(game: game, sourceID: id, name: "Sol Ring", setName: "Commander Masters", setCode: "CMM", number: "395",
         thumbnailURL: URL(string: "https://example.com/\(id).jpg"), finishes: [.nonfoil, .foil])
}
