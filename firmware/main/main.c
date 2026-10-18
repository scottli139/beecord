// Beecord 固件入口（M0 骨架）。
// 当前仅打印启动日志验证工程链路；BSP、音频、存储、BLE 等硬件初始化
// 在后续里程碑按需加入。
#include "esp_log.h"

static const char *TAG = "beecord";

#define FW_VERSION "0.1.0-m0"

void app_main(void)
{
    ESP_LOGI(TAG, "Beecord firmware %s", FW_VERSION);
}
