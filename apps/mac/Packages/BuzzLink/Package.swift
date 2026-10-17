// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "BuzzLink",
    platforms: [.macOS(.v15)],
    products: [
        .library(name: "BuzzLinkCore", targets: ["BuzzLinkCore"]),
        .library(name: "BuzzLinkSimCore", targets: ["BuzzLinkSimCore"]),
        .library(name: "BuzzLink", targets: ["BuzzLink"]),
        .library(name: "BuzzLinkSync", targets: ["BuzzLinkSync"]),
        .library(name: "BuzzLinkAudio", targets: ["BuzzLinkAudio"])
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
        // 音频链（T6）：ogg→m4a 转码 + 播放器引擎（AVFoundation；PlayerEngine 逻辑可注入时钟单测）
        .target(
            name: "BuzzLinkAudio",
            dependencies: ["BuzzLinkCore"]
        ),
        .testTarget(
            name: "BuzzLinkAudioTests",
            dependencies: ["BuzzLinkAudio", "BuzzLinkCore"],
            resources: [.process("Fixtures")]
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
