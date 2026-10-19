// Beecord 固件入口（M0 骨架 + M1 自检探针）。
// 当前打印启动日志并探测 TF 卡；BSP 显示/音频/BLE/UI 等初始化随 M2~M5 按需加入。
#include "buzz_sdcard.h"
#include "esp_log.h"

static const char *TAG = "beecord";

#define FW_VERSION "0.1.1-m1"

void app_main(void)
{
    ESP_LOGI(TAG, "Beecord firmware %s", FW_VERSION);
    buzz_sdcard_probe();
}
