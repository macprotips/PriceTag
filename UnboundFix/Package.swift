// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "UnboundFix",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "UnboundFix", targets: ["UnboundFix"]),
    ],
    targets: [
        // Finding, checking, backing up, patching and restoring the DLL. No UI, so it is easy to test.
        .target(name: "UnboundFixKit"),
        // The SwiftUI app.
        .executableTarget(name: "UnboundFix", dependencies: ["UnboundFixKit"]),
        .testTarget(name: "UnboundFixKitTests", dependencies: ["UnboundFixKit"]),
    ]
)
