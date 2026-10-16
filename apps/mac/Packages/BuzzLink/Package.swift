// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BuzzLink",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "BuzzLinkCore", targets: ["BuzzLinkCore"]),
        .library(name: "BuzzLinkSimCore", targets: ["BuzzLinkSimCore"]),
        .library(name: "BuzzLink", targets: ["BuzzLink"]),
        .library(name: "BuzzLinkSync", targets: ["BuzzLinkSync"])
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
        // 同步引擎（录音库写入 + 自动同步/队列/进度/重连；纯 Foundation，无 UI 依赖）
        .target(
            name: "BuzzLinkSync",
            dependencies: ["BuzzLink", "BuzzLinkCore"]
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
        ),
        .testTarget(
            name: "BuzzLinkSyncTests",
            dependencies: ["BuzzLinkSync", "BuzzLink", "BuzzLinkSimCore", "BuzzLinkTestSupport"]
        )
    ],
    swiftLanguageModes: [.v6]
)
