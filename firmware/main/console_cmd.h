// Beecord USB 串口 REPL（M2）：esp_console 注册录控命令并启动交互式控制台。
//
// 命令集：
//   rec start   开始录音（递增序号 WAV 写 TF 卡）
//   rec stop    停止录音（回填长度、关闭文件并回报统计）
//   ls          列出 TF 卡根目录文件与剩余空间
//   get         base64 导出 TF 卡文件（串口传输）
//   gain        设置麦克风增益 dB
//   ble         BLE 状态（设备名/连接/MTU/订阅）
// 命令解析失败/参数缺失打印用法提示；状态非法（重复 start / 空闲 stop）给友好提示不崩溃。
// REPL 复用 M1 已验证的 USB Serial/JTAG 控制台，M3/M4 继续扩展。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 esp_console、注册命令并启动 USB Serial/JTAG REPL（REPL 在独立任务运行，本函数立即返回）。
// 重复调用返回 ESP_ERR_INVALID_STATE。
esp_err_t console_cmd_start_repl(void);

#ifdef __cplusplus
}
#endif
