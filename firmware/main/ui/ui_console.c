// UI 相关 REPL 命令实现（ui_console.c，M5.1）。
#include "ui_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_check.h"

#include "screen_pwr.h"
#include "sys/beep_play.h"
#include "ui_snapshot.h"
#include "ui_task.h"

static const char *TAG_SCREEN = "ui_console";

static int cmd_screen(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "off") != 0 && strcmp(argv[1], "on") != 0)) {
        printf("Usage:\n  screen off|on  熄屏/亮屏（调试）\n");
        return 1;
    }
    if (!ui_is_ready()) {
        printf("UI 未初始化\n");
        return 1;
    }
    bool off = strcmp(argv[1], "off") == 0;
    screen_pwr_set_on(!off);
    printf("screen %s\n", off ? "off" : "on");
    return 0;
}

// 提示音试听：beep <0-6> 事件音（ux-device §8）；beep t <freq_hz> <dur_ms> <vol>
// 自定义单音（声学特性刻画/音量听辨，TESTING 调试手段长期保留）
static int cmd_beep(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n  beep <0-6>        试听提示音（0开始 1停止 2暂停 3继续 4标记 5错误 6连接）\n"
               "  beep t <freq> <dur> <vol>  自定义单音（如 beep t 1000 1000 60）\n");
        return 1;
    }
    if (strcmp(argv[1], "t") == 0) {
        if (argc < 5) {
            printf("用法: beep t <freq_hz> <dur_ms> <vol 0-100>\n");
            return 1;
        }
        int freq = atoi(argv[2]);
        int dur = atoi(argv[3]);
        int vol = atoi(argv[4]);
        if (freq < 100 || freq > 8000 || dur < 20 || dur > 2000 || vol < 0 || vol > 100) {
            printf("参数范围: freq 100-8000 / dur 20-2000ms / vol 0-100\n");
            return 1;
        }
        esp_err_t ret = beep_play_tone((uint16_t)freq, (uint16_t)dur, vol);
        if (ret != ESP_OK) {
            printf("beep t 失败: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("beep t %d %d %d ok\n", freq, dur, vol);
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n >= (int)BEEP_EVT_COUNT) {
        printf("事件号须为 0~%d\n", (int)BEEP_EVT_COUNT - 1);
        return 1;
    }
    esp_err_t ret = beep_play((beep_event_t)n);
    if (ret != ESP_OK) {
        printf("beep 失败: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("beep %d ok\n", n);
    return 0;
}

static int cmd_ui(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "touch") == 0) {
        ui_debug_touch_dump();
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "tile") == 0) {
        // 调试导航：ui tile <col 0-2> <row 0-1>（row1=设置页）
        int col = atoi(argv[2]);
        int row = atoi(argv[3]);
        if (col < 0 || col > 2 || row < 0 || row > 1) {
            printf("tile 坐标越界：col 0-2 / row 0-1\n");
            return 1;
        }
        ui_nav_to_tile((uint8_t)col, (uint8_t)row);
        printf("tile %d,%d\n", col, row);
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "snap") != 0) {
        printf("Usage:\n  ui snap   截当前屏幕存 /sdcard/snap.bmp（配合 get snap.bmp 导出）\n"
               "  ui touch  打印触摸控制器原始触点（摸屏时连发观察）\n"
               "  ui tile <c 0-2> <r 0-1>  调试跳页（r1=设置页）\n");
        return 1;
    }
    if (!ui_is_ready()) {
        printf("UI 未初始化\n");
        return 1;
    }
    // 快照渲染栈需求大（console_repl 16KB 实测仍溢出）→ 派发到 LVGL 任务执行
    esp_err_t ret = ui_snapshot_request_async();
    if (ret == ESP_OK) {
        ret = ui_snapshot_wait_done(15000);
    }
    if (ret != ESP_OK) {
        printf("snap 失败: %s\n", esp_err_to_name(ret));
        return 1;
    }
    return 0;
}

esp_err_t ui_console_register(void)
{
    const esp_console_cmd_t screen_cmd = {
        .command = "screen",
        .help = "熄屏/亮屏（调试路径）",
        .hint = "off|on",
        .func = &cmd_screen,
    };
    const esp_console_cmd_t ui_cmd = {
        .command = "ui",
        .help = "UI 调试（截图/触摸原始值）",
        .hint = "snap|touch",
        .func = &cmd_ui,
    };
    const esp_console_cmd_t beep_cmd = {
        .command = "beep",
        .help = "试听提示音（0-6 事件 / t <freq> <dur> <vol> 自定义单音）",
        .hint = "<0-6>|t <freq> <dur> <vol>",
        .func = &cmd_beep,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&screen_cmd), TAG_SCREEN,
                        "register screen failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ui_cmd), TAG_SCREEN,
                        "register ui failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&beep_cmd), TAG_SCREEN,
                        "register beep failed");
    return ESP_OK;
}
