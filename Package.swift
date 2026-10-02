// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "PriceTag",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "PriceTag", targets: ["PriceTag"]),
    ],
    targets: [
        // Formatting, the font and the renderer. No UI, so it is easy to test.
        .target(name: "PriceTagKit"),
        // The SwiftUI app.
        .executableTarget(name: "PriceTag", dependencies: ["PriceTagKit"]),
        .testTarget(name: "PriceTagKitTests", dependencies: ["PriceTagKit"]),
    ]
)
