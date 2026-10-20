// UI 相关 REPL 命令实现（ui_console.c，M5.1）。
#include "ui_console.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_check.h"

#include "screen_pwr.h"
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

static int cmd_ui(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "touch") == 0) {
        ui_debug_touch_dump();
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "snap") != 0) {
        printf("Usage:\n  ui snap   截当前屏幕存 /sdcard/snap.bmp（配合 get snap.bmp 导出）\n"
               "  ui touch  打印触摸控制器原始触点（摸屏时连发观察）\n");
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
        .help = "UI 调试（截图）",
        .hint = "snap",
        .func = &cmd_ui,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&screen_cmd), TAG_SCREEN,
                        "register screen failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ui_cmd), TAG_SCREEN,
                        "register ui failed");
    return ESP_OK;
}
