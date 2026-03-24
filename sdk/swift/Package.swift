// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SolunaSDK",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [
        .library(name: "SolunaSDK", targets: ["SolunaSDK"]),
    ],
    targets: [
        .target(name: "SolunaSDK"),
        .testTarget(name: "SolunaSDKTests", dependencies: ["SolunaSDK"]),
    ]
)
