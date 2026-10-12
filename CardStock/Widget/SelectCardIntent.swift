import AppIntents
import CardStockKit
import WidgetKit

/// Lets you pick which card the small widget shows.
struct SelectCardIntent: WidgetConfigurationIntent {
    static var title: LocalizedStringResource { "Choose Card" }
    static var description: IntentDescription {
        IntentDescription("Pick a card from your watchlist. Leave it empty to show your biggest mover.")
    }

    @Parameter(title: "Card")
    var card: CardEntity?

    init() {}
}

struct CardEntity: AppEntity {
    static var typeDisplayRepresentation: TypeDisplayRepresentation { TypeDisplayRepresentation(name: "Card") }
    static var defaultQuery: CardEntityQuery { CardEntityQuery() }

    var id: String
    var name: String
    var subtitle: String

    var displayRepresentation: DisplayRepresentation {
        DisplayRepresentation(title: "\(name)", subtitle: "\(subtitle)")
    }

    init(row: WidgetSnapshot.Row) {
        id = row.id
        name = row.name
        subtitle = row.subtitle
    }
}

struct CardEntityQuery: EntityQuery {
    func entities(for identifiers: [CardEntity.ID]) async throws -> [CardEntity] {
        rows().filter { identifiers.contains($0.id) }.map(CardEntity.init(row:))
    }

    func suggestedEntities() async throws -> [CardEntity] {
        rows().map(CardEntity.init(row:))
    }

    private func rows() -> [WidgetSnapshot.Row] {
        WidgetSnapshot.load()?.rows ?? []
    }
}
