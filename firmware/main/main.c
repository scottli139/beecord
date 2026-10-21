// Beecord 固件入口（M0 骨架 + M1 自检探针 + M2 音频采集与 REPL 录控 +
// M3 编码封装 + M4.1 BLE 基础设施 + M4.3 控制通道命令）。
// 启动序列：版本打印 → NVS 初始化 → TF 卡探测挂载 → 麦克风初始化（16kHz/16bit/双麦）
// → BLE 初始化（BCD-XXXX 广播）→ BLE 命令服务（buzz_cmds）→ USB 串口 REPL。
// 各阶段失败降级：音频失败则 rec 命令提示不可用，BLE 失败仅失能同步功能，
// REPL 始终启动保证可交互。
#include "audio/rec_journal_esp.h"
#include "buzz_audio.h"
#include "buzz_ble.h"
#include "buzz_cmds.h"
#include "buzz_recorder.h"
#include "buzz_sdcard.h"
#include "console_cmd.h"
#include "esp_log.h"
#include "fw_version.h"
#include "nvs_flash.h"
#include "ui/ui_rec.h"
#include "ui/ui_task.h"

static const char *TAG = "beecord";

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

    // M5.3：断电恢复检测（journal 残留 → 截断收尾 + 置恢复标志，UI 起来后 toast）
    {
        char recovered[80] = { 0 };
        bool was_recovered = false;
        if (rec_journal_check_recovery(recovered, sizeof(recovered),
                                       &was_recovered) == ESP_OK &&
            was_recovered) {
            ui_rec_set_recovered(recovered);
        }
    }

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
    } else {
        ESP_LOGI(TAG, "BLE device: %s", buzz_ble_device_name());
        // M4.3：控制通道命令服务（INFO/LIST/FILE_INFO）；失败仅失能 BLE 命令。
        ret = buzz_cmds_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE commands init failed (%s), sync disabled",
                     esp_err_to_name(ret));
        }
    }

    // M5.1：display + LVGL + 手表 UI 框架（失败仅无 UI，REPL 仍可用；
    // SD 挂载失败现状行为不变——格式化弹窗是 M5.5 的事）
    ret = ui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed (%s), headless mode", esp_err_to_name(ret));
    }

    if (console_cmd_start_repl() != ESP_OK) {
        ESP_LOGE(TAG, "console REPL start failed");
    }
}
