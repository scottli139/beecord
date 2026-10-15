// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BuzzLink",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "BuzzLinkCore", targets: ["BuzzLinkCore"]),
        .library(name: "BuzzLinkSimCore", targets: ["BuzzLinkSimCore"]),
        .library(name: "BuzzLink", targets: ["BuzzLink"])
    ],
    targets: [
        .target(name: "BuzzLinkCore"),
        .target(
            name: "BuzzLinkSimCore",
            dependencies: ["BuzzLinkCore"]
        ),
        .target(
            name: "BuzzLink",
            dependencies: ["BuzzLinkCore"]
        ),
        .testTarget(
            name: "BuzzLinkCoreTests",
            dependencies: ["BuzzLinkCore"],
            resources: [.process("Fixtures")]
        ),
        .testTarget(
            name: "BuzzLinkSimCoreTests",
            dependencies: ["BuzzLinkSimCore", "BuzzLinkCore"]
        ),
        .testTarget(
            name: "BuzzLinkTests",
            dependencies: ["BuzzLink", "BuzzLinkCore", "BuzzLinkSimCore"]
        )
    ],
    swiftLanguageModes: [.v6]
)
