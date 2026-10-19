// Beecord 固件入口（M0 骨架 + M1 自检探针 + M2 音频采集与 REPL 录控 +
// M3 编码封装 + M4.1 BLE 基础设施）。
// 启动序列：版本打印 → NVS 初始化 → TF 卡探测挂载 → 麦克风初始化（16kHz/16bit/双麦）
// → BLE 初始化（BCD-XXXX 广播）→ USB 串口 REPL（rec/ls/get/gain/ble）。
// 各阶段失败降级：音频失败则 rec 命令提示不可用，BLE 失败仅失能同步功能，
// REPL 始终启动保证可交互。
#include "buzz_audio.h"
#include "buzz_ble.h"
#include "buzz_recorder.h"
#include "buzz_sdcard.h"
#include "console_cmd.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "beecord";

#define FW_VERSION "0.4.0-m4"

void app_main(void)
{
    ESP_LOGI(TAG, "Beecord firmware %s", FW_VERSION);

    // NVS：PHY 校准数据与 NimBLE 持久化的依赖；无可用页/版本不兼容则擦除重建。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    buzz_sdcard_probe();

    esp_codec_dev_handle_t mic = NULL;
    if (buzz_audio_open_mic(&mic) != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed, rec unavailable");
    } else if (buzz_recorder_init(mic) != ESP_OK) {
        ESP_LOGE(TAG, "recorder init failed, rec unavailable");
    }

    // BLE 失败仅降级（同步功能不可用），不阻断 REPL。
    ret = buzz_ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed (%s), sync disabled", esp_err_to_name(ret));
    }

    if (console_cmd_start_repl() != ESP_OK) {
        ESP_LOGE(TAG, "console REPL start failed");
    }
}
