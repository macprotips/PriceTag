// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SM2Fix",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "SM2Fix", targets: ["SM2Fix"]),
    ],
    targets: [
        // Finding the game and its bottle, installing and removing the fix. No UI, so it is easy to test.
        .target(name: "SM2FixKit"),
        // The SwiftUI app.
        .executableTarget(name: "SM2Fix", dependencies: ["SM2FixKit"]),
        .testTarget(name: "SM2FixKitTests", dependencies: ["SM2FixKit"]),
    ]
)
