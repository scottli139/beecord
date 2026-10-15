// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BuzzLink",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "BuzzLinkCore", targets: ["BuzzLinkCore"]),
        .library(name: "BuzzLinkSimCore", targets: ["BuzzLinkSimCore"])
    ],
    targets: [
        .target(name: "BuzzLinkCore"),
        .target(
            name: "BuzzLinkSimCore",
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
        )
    ],
    swiftLanguageModes: [.v6]
)
