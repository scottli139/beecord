<p align="center">
  <img src="docs/images/hero.png" alt="Beecord — Record on your wrist. Transcribe on your Mac.">
</p>

# Beecord

Open-source AI voice recorder: watch-form firmware + local-first companion apps.

- **Firmware** for the Waveshare ESP32-S3-Touch-AMOLED-2.06 board — records 16 kHz mono
  Ogg Opus to a microSD card, syncs files to a computer over BLE.
- **BuzzLink** — a small, documented BLE file-sync protocol (frame magic `BE EC`) used
  between device and apps.
- **Beecord Studio** — macOS companion app: browse & download recordings, playback,
  on-device transcription (whisper.cpp), optional cloud-AI summaries via
  OpenAI-compatible endpoints (Kimi, DeepSeek, ...).

> Status: early development (pre-M0). Docs: [BuzzLink protocol spec](docs/BUZZLINK.md) ·
> [Studio tech design](docs/studio-tech-design.md) · [Coding standards](docs/CODING_STANDARDS.md) ·
> [Testing](docs/TESTING.md)

## 简介（中文）

Beecord 是开源的 AI 录音手表项目：基于微雪 ESP32-S3-Touch-AMOLED-2.06 开发板的固件
+ 本地优先的 macOS 客户端（Beecord Studio）。设备本地录音存 TF 卡，经 BuzzLink
协议（BLE）同步到电脑；支持本地 whisper 转写，以及可选的云端 AI 摘要/纪要
（OpenAI 兼容端点，内容出域需显式开启）。项目处于早期开发阶段。

## Design previews

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/mac-app-dark.png">
  <img src="docs/images/mac-app-light.png" width="900" alt="Beecord Studio for macOS — recording library, player, on-device transcript">
</picture>

<img src="docs/images/watch-screens.png" width="1100" alt="Watch UI — watch face, recording, BLE sync, recordings">

<img src="docs/images/flow.png" width="1200" alt="Pipeline — record on watch, BuzzLink BLE sync, local whisper transcription, optional cloud AI summary">

> 交互与视觉定稿：[ux-design](docs/ux-design.md) · [ux-device](docs/ux-device.md) ·
> [DESIGN_SYSTEM](docs/DESIGN_SYSTEM.md) · [BRAND](docs/BRAND.md) ·
> 渲染图源（HTML，无头截图生成）：[`docs/images/src/`](docs/images/src/)

## Repository layout

| Path | Content |
|---|---|
| `firmware/` | ESP-IDF project (ESP32-S3, NimBLE, LVGL) |
| `apps/mac/` | Beecord Studio (SwiftUI, CoreBluetooth) |
| `sdk/python/`, `sdk/swift/` | BuzzLink client SDKs |
| `docs/` | BuzzLink protocol spec, guides |
| `tools/` | Development utilities |

## License

[Apache-2.0](LICENSE)

Commercial use is welcome — see [docs/COMMERCIAL.md](docs/COMMERCIAL.md) for what you must
handle yourself (certifications, privacy law, patents). This project is provided as-is;
nothing here is legal advice.
