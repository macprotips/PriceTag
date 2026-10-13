import CardStockKit
import Foundation
import UserNotifications

/// Posts a notification when a price alert goes off.
enum AlertNotifier {
    /// Asks once, the first time someone sets an alert.
    static func requestPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    static func post(_ hits: [AlertHit]) {
        guard !hits.isEmpty else { return }
        let center = UNUserNotificationCenter.current()
        for hit in hits {
            let content = UNMutableNotificationContent()
            content.title = hit.cardName
            content.subtitle = hit.subtitle
            let price = PriceFormat.price(hit.price, hit.currency)
            let level = PriceFormat.price(hit.level, hit.currency)
            content.body = switch hit.kind {
            case .above: "Now \(price), above your \(level) alert."
            case .below: "Now \(price), below your \(level) alert."
            }
            content.sound = .default
            content.userInfo = ["url": DeepLink.card(hit.itemID).absoluteString]
            center.add(UNNotificationRequest(identifier: "\(hit.itemID)-\(hit.kind)-\(hit.level.cents)", content: content, trigger: nil))
        }
    }
}
