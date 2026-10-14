# BuzzLink Protocol (draft)

BuzzLink 是 Beecord 设备与客户端之间的 BLE 文件同步协议。设计目标：简单、可靠、
可跨平台实现的批量文件传输（录音笔离机录音数小时后一次性同步）。

- **传输**：BLE GATT，单一私有服务（write + notify 特征对），MTU 247，连接间隔调优。
- **帧**：`BE EC` 魔数开头的小头帧（type / session / channel / length / payload / crc16）。
- **通道**：JSON 控制通道（列文件/查信息）+ 二进制文件传输通道（开始/数据流水/结束校验）。
- **文件格式**：Ogg Opus（16 kHz 单声道 ~44 kbps），整块流水推送，整文件
  CRC-16/CCITT-FALSE 验收。

> 规范全文随 M4 里程碑补全；当前处于设计阶段，以 `firmware/main/proto/` 的实现为准。
> 协议借鉴了 BLE 文件同步领域的常见做法，帧定义、命令集与服务 UUID 均为本项目自有。
