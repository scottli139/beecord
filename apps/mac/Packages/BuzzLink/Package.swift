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
        // 测试支撑（不进 products）：进程内 Sim 对端传输与进度收集器，供各测试 target 复用
        .target(
            name: "BuzzLinkTestSupport",
            dependencies: ["BuzzLink", "BuzzLinkSimCore", "BuzzLinkCore"]
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
            dependencies: ["BuzzLink", "BuzzLinkCore", "BuzzLinkSimCore", "BuzzLinkTestSupport"]
        )
    ],
    swiftLanguageModes: [.v6]
)
