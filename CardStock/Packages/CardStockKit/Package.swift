// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CardStockKit",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "CardStockKit", targets: ["CardStockKit"]),
    ],
    targets: [
        // Models, price sources, storage and history maths. No UI, so the app
        // and the widget share it and it is tested with `swift test`.
        .target(
            name: "CardStockKit",
            swiftSettings: [.enableExperimentalFeature("StrictConcurrency")]
        ),
        .testTarget(
            name: "CardStockKitTests",
            dependencies: ["CardStockKit"],
            resources: [.copy("Fixtures")]
        ),
    ]
)
