# CardStock: plan

A Mac app and desktop widget that track the prices of specific Magic: The Gathering and Pokémon cards, the way Stocks tracks shares.

"CardStock" is a working name. It's set in one place (`project.yml`), so changing it is cheap.

---

## 1. Product

### Who it's for

Collectors and content creators who keep an eye on a few dozen cards. They want to glance at a widget and see what moved, and open the app to see why. It isn't for running a store, valuing a whole collection or trading.

### Principles

1. **Glanceable first.** The widget is the main surface and the app is where you go for detail. Each row answers "what is it worth, and which way is it going?" in under a second.
2. **One obvious way to do things.** Add a card with ⌘N, type, press Return. That's it. No accounts, no onboarding carousel, no settings you need to touch first.
3. **Honest data.** Every price says where it came from and when. If a price is stale or missing, the app says so instead of hiding it.
4. **Looks like it shipped with the Mac.** System fonts, SF Symbols, standard controls, materials and system colors. No gradients for decoration, no glows, no emoji, no custom chrome.
5. **Legal by construction.** Only official, documented APIs that allow this use. No scraping.

### v1 scope

| In | Out (for now) |
| --- | --- |
| Watchlist of specific printings, each with a finish (normal, foil, holo…) | Collection or portfolio valuation |
| Search Magic and Pokémon cards and add them | Graded card (PSA, BGS) prices |
| Price, day change and change since added | Sealed product |
| Price chart for 1W, 1M, 3M, 6M, 1Y and All | Alerts and notifications (v1.1) |
| Card image, set, number and rarity | iOS and iPad (the shared code is ready for it) |
| Links to buy on TCGplayer and Cardmarket | Menu bar ticker (v1.1) |
| Small, medium and large widgets | Accounts and sync |
| USD or EUR | Other games (Lorcana, One Piece…) |

---

## 2. Data sources and legality

This decides the whole architecture, so it comes first.

### What we use

| Game | Source | Cost | Key | Prices it gives | Terms that matter |
| --- | --- | --- | --- | --- | --- |
| Magic | [Scryfall API](https://scryfall.com/docs/api) | Free | None | TCGplayer market (USD, normal/foil/etched), Cardmarket (EUR, normal/foil), MTGO tix. Updated once a day. | Send a `User-Agent` and `Accept` header. Keep to the rate limits (we do 2 requests a second at most). Cache data rather than re-fetching it. Don't paywall Scryfall data. Don't crop or cover the artist credit or copyright on card images. Treat prices more than a day old as stale. |
| Pokémon | [TCGdex API](https://tcgdex.dev) | Free, open source | None | TCGplayer (USD, per variant: low, mid, high, market) and Cardmarket (EUR: average, low, trend, and 1, 7 and 30-day averages, normal and holo). TCGplayer updates hourly, Cardmarket daily. | Open data (MIT). Not affiliated with Nintendo or The Pokémon Company. |

### What we don't use, and why

| Source | Why not |
| --- | --- |
| Scraping TCGplayer, Cardmarket, eBay or PriceCharting | Against their terms of service. Also fragile: one HTML change breaks the app. |
| TCGplayer API | Closed to new developers. |
| pokemontcg.io | Deprecated. New sign-ups are closed and existing keys stop working on 1 March 2027. Its successor, Scrydex, is paid. |
| Scrydex, JustTCG, PriceCharting API | Good data, including price history, but paid. They're the planned optional "bring your own key" source (see Phase 3). |

### Price history

Neither free source gives history, only today's price. So:

1. **The app records its own history.** Each refresh saves one point per card, per day. Charts get richer the longer a card is watched.
2. **Pokémon gets trend context on day one.** Cardmarket's 1, 7 and 30-day averages from TCGdex give a 7D and 30D change before we have our own history.
3. **The UI is honest about it.** A new card shows "History starts today" with a single point, not a fake line.
4. **Later, optional backfill.** An optional paid source (Phase 3) can backfill up to a year of history for people who want it.

### Attribution

Settings → Data shows each source with a link. Each card's detail view says "Prices from Scryfall" or "Prices from TCGdex", with the time it was updated. Card images are always shown whole, never cropped, so artist credits stay visible.

---

## 3. Architecture

### Targets

```
CardStock.xcodeproj  (generated from project.yml by XcodeGen)
├── CardStock         macOS app, SwiftUI, macOS 14+
├── CardStockWidget   WidgetKit extension: small, medium, large
└── CardStockKit      local Swift package, used by both
    ├── Model         Card, WatchItem, PricePoint, Quote, Finish, Game
    ├── Providers     PriceProvider protocol, Scryfall, TCGdex, HTTP, rate limiter
    ├── Store         WatchlistStore (actor), history maths, widget snapshot
    └── Formatting    currency and percent formatting
```

All the logic lives in **CardStockKit**, which has no UI code. It builds with `swift build` and is tested with `swift test`, so most of the app can be tested without Xcode or a simulator.

### Data flow

```
          ┌────────────── App Group container ──────────────┐
          │  watchlist.json       (items + history)         │
          │  widget-snapshot.json (small, ready to draw)    │
          │  Thumbnails/          (card images for widgets) │
          └─────────▲─────────────────────────▲─────────────┘
                    │ read/write              │ read (+ refresh if stale)
  ┌─────────────────┴───────┐      ┌──────────┴──────────────┐
  │ CardStock app           │      │ CardStockWidget         │
  │ AppModel (@Observable)  │      │ TimelineProvider        │
  │   → PriceRefresher      │      │   → PriceRefresher      │
  │   → WatchlistStore      │      │   → WidgetSnapshot      │
  └─────────────────┬───────┘      └──────────┬──────────────┘
                    └──────────┬──────────────┘
                     ┌─────────┴─────────┐
                     │ PriceProvider     │
                     │  ├ Scryfall       │
                     │  └ TCGdex         │
                     └───────────────────┘
```

- **One writer at a time.** `WatchlistStore` is an actor and writes atomically. The app and the widget never write at the same moment in practice (the widget only refreshes when the data is more than six hours old and the app isn't doing it), and an atomic write means the worst case is one lost refresh, never a corrupt file.
- **The widget doesn't decode the whole watchlist.** The app writes `widget-snapshot.json`: just the rows and the downsampled sparkline points the widget draws. That keeps the widget fast and well under its memory limit.
- **The widget can refresh on its own.** If the Mac has been on for days without the app open, the widget's timeline provider refreshes prices itself. Prices change daily at most, so it asks to be reloaded every few hours.
- **After every refresh** the app calls `WidgetCenter.shared.reloadAllTimelines()`.

### Why these choices

| Decision | Alternatives | Why |
| --- | --- | --- |
| macOS 14+ | 13 | `@Observable`, widgets configured with App Intents, chart selection in Swift Charts. Sonoma is three years old. |
| JSON file in the App Group | SwiftData, Core Data, SQLite | A watchlist is small (hundreds of cards, a year of daily points is about 10 MB at the very most). A file is easy to read from the widget, easy to back up, easy to test on any platform and has no migration framework. If it outgrows that, `WatchlistStore` is the only thing that changes. |
| XcodeGen | Checked-in `.xcodeproj` | The project file is readable and reviewable, has no merge conflicts and builds in CI. |
| No third-party dependencies | Alamofire, Kingfisher, GRDB | URLSession, Swift Charts and AsyncImage do the job. Fewer things to update and audit. |
| Daily history points | Every refresh | Sources update daily. More points would just be noise and file size. |

### Identity

A tracked item is a **printing plus a finish**: "Sol Ring, Commander Masters #395, foil" is a different item from the non-foil one. Its ID is `game:sourceID:finish`, for example `mtg:5e1f…:foil` or `pokemon:swsh3-136:holofoil`.

### Prices, precisely

- Prices are stored as `Decimal`, never `Double`, so $0.10 is exactly $0.10.
- **Magic:** USD is TCGplayer market price; EUR is Cardmarket trend. Both come from Scryfall.
- **Pokémon:** USD is TCGplayer market price for the chosen variant, falling back to mid. EUR is Cardmarket trend, falling back to the 30-day average (trend can be 0 on thin markets).
- **Day change** compares with the most recent point from an earlier day. Until there is one, it shows "—", not +0.00%.

### Refresh and rate limits

- Scryfall's `/cards/collection` takes up to 75 cards per request, so a 200-card watchlist refreshes in 3 requests.
- TCGdex has no batch endpoint, so cards are fetched one at a time, a few at once, with a limit on concurrency.
- A shared rate limiter keeps each source under its published limit.
- The app refreshes on launch if prices are more than an hour old, then every 6 hours while it's running, and whenever you press ⌘R.
- Failures are per card. One bad card never blocks the rest, and the row shows when it was last updated.

---

## 4. Design

The reference is Stocks on macOS, along with Mail, Notes and System Settings. The aim is that someone could believe Apple made it.

### Window

```
┌──────────────────────────────────────────────────────────────────────────┐
│ ● ● ●                                  ⟳   +                             │
├────────────────────────────┬─────────────────────────────────────────────┤
│ Watchlist          ↑↓      │  Sheoldred, the Apocalypse                  │
│                            │  Dominaria United · #107 · Foil             │
│ ▢ Sheoldred, the Apo…  ╱╲╱ │                                             │
│   DMU · Foil      $84.20   │  $84.20                          ┌────────┐ │
│                  [+2.4%]   │  +$1.97 (+2.4%) today            │        │ │
│ ▢ Charizard ex        ╲╱╲  │                                  │  card  │ │
│   OBF · Holo     $27.15    │  1W  1M  [3M]  6M  1Y  All       │ image  │ │
│                  [−1.1%]   │  ┌──────────────────────────┐    │        │ │
│ ▢ Sol Ring           ──╱   │  │           ╱╲      ╱╲╱    │    └────────┘ │
│   CMM · Normal    $1.49    │  │     ╱╲╱╲╱   ╲╱╲╱         │               │
│                  [+0.7%]   │  └──────────────────────────┘               │
│                            │  Low  $79.10   High  $86.00   Added  $81.00 │
│                            │  TCGplayer  $84.20   Cardmarket  €71.30     │
│                            │                                             │
│                            │  Buy on TCGplayer ↗   Cardmarket ↗          │
│                            │  Prices from Scryfall · Updated 2h ago      │
└────────────────────────────┴─────────────────────────────────────────────┘
```

- **Sidebar rows** follow Stocks: a thumbnail, name on top and set code plus finish underneath in secondary text, a sparkline, the price in monospaced digits, and a change pill. Clicking the pill switches every row between percent, amount and price, like Stocks.
- **Detail:** the title in large semibold, the price at 34 pt with monospaced digits, and the change in green or red. A segmented control picks the range. The chart is a single line with a very light fill under it, colored by whether the range is up or down. Hovering shows a vertical rule with the date and price, like Stocks.
- **Card image:** the whole card, with corners matching a real card (about 4.7% of the width) and a soft shadow. Clicking it opens a Quick Look style enlargement.
- **Stats:** a two-column grid of label and value, labels in secondary text. No boxes or cards around them.
- **Empty states** use `ContentUnavailableView`: "No Cards" with an "Add Card" button, and "No Results" for search.

### Adding a card

⌘N or the + button opens a sheet with a search field in focus and a Magic / Pokémon picker. Results appear as you type (with a 250 ms pause), each with a thumbnail, name, set, number and price. ↑↓ moves through them and Return adds the selected one. If a card comes in more than one finish, a small menu on the row picks it, defaulting to the one with a price.

### Typography and color

- SF Pro throughout, through the system text styles (`.largeTitle`, `.headline`, `.subheadline`…), so Dynamic Type and accessibility settings work.
- `.monospacedDigit()` on every number, so columns don't jitter when prices change.
- System green and red for change. No other accent besides the system accent color.
- Materials only where macOS uses them (sidebar, sheet).
- Light and dark mode, and Increase Contrast, all work, because everything is a semantic color.

### Widgets

| Size | Content |
| --- | --- |
| Small | One card: thumbnail, name, price, change and a sparkline. You pick the card in the widget's settings, or it shows your top mover. |
| Medium | Three or four watchlist rows: name and set, sparkline, price and change pill. Like the Stocks medium widget. |
| Large | Seven or eight rows. |

Widgets use the system widget background and `containerBackground`, and look right when macOS dims them on the desktop. Tapping a row opens that card in the app (`cardstock://card/<id>`).

### Keyboard

| Shortcut | Action |
| --- | --- |
| ⌘N | Add Card |
| ⌘R | Refresh Prices |
| Delete | Remove the selected card (with Undo) |
| ⌘1 to ⌘6 | Chart range 1W, 1M, 3M, 6M, 1Y, All |
| ⌘⌥S | Show or hide the sidebar |
| ⌘, | Settings |

### Accessibility

- Every row reads as one VoiceOver element, for example: "Sheoldred, the Apocalypse, Dominaria United foil, 84 dollars 20, up 2.4 percent."
- Charts have audio graphs and a summary label.
- Up and down are never shown by color alone: there's always a sign, and an arrow in the pill.

---

## 5. Engineering practice

- **Tests:** CardStockKit has unit tests for decoding real API responses (saved as fixtures), choosing prices, history maths (ranges, changes, daily de-duplication, downsampling) and store round trips. These run on every push.
- **CI:** GitHub Actions on macOS runs `swift test` for the package, generates the project with XcodeGen and builds the app and widget.
- **Concurrency:** Swift 6 strict concurrency. Stores and providers are actors or `Sendable`.
- **Errors:** typed errors from providers. The UI shows a calm inline message ("Couldn't reach Scryfall. Showing prices from 3 hours ago.") rather than an alert.
- **Privacy:** no analytics, no accounts. The only network requests go to the two price sources and their image servers.
- **Distribution:** Developer ID signing and notarization for direct download. The Mac App Store is possible later, since everything is sandboxed already.

---

## 6. Roadmap

### Phase 1: foundation (this change)

- [x] Plan and design spec
- [x] CardStockKit: models, Scryfall and TCGdex providers, store, history, formatting, tests
- [x] App: watchlist, detail with chart, add card, refresh, settings
- [x] Widgets: small, medium, large
- [x] CI that builds and tests everything

### Phase 2: polish (v1.0)

- [ ] App icon, designed properly (not generated)
- [ ] Interactive widget: refresh button, choose a card in the small widget
- [ ] Clicking the card image enlarges it
- [ ] Undo for removing cards
- [ ] Reordering the watchlist by drag and drop
- [ ] Import and export the watchlist (CSV and JSON)
- [ ] Notarized download from GitHub Releases

### Phase 3: v1.1

- [ ] Price alerts ("tell me when this is above $100") as local notifications
- [ ] Menu bar ticker
- [ ] Quantity and price paid, for a simple gain/loss view
- [ ] Optional history backfill with your own Scrydex or JustTCG key

### Later

- [ ] iPhone and iPad app with the same CardStockKit, synced through iCloud
- [ ] More games (Lorcana, One Piece, Yu-Gi-Oh!) as sources allow
