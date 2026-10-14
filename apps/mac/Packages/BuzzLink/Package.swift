// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BuzzLink",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "BuzzLinkCore", targets: ["BuzzLinkCore"])
    ],
    targets: [
        .target(name: "BuzzLinkCore"),
        .testTarget(
            name: "BuzzLinkCoreTests",
            dependencies: ["BuzzLinkCore"],
            resources: [.process("Fixtures")]
        )
    ],
    swiftLanguageModes: [.v6]
)
