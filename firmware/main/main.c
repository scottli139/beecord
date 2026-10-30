// Beecord 固件入口（M0 骨架 → M5.4 收尾）。启动序列（2026-10-30 实际顺序）：
// 版本打印 → NVS → settings/sync_marks（NVS）→ TF 卡挂载 → 音频（bsp i2c 随之就绪）
// → 设置应用（质量/增益/提示音/熄屏）→ PCF85063 RTC（须在 BLE 前，TIME_SET 窗口竞态）
// → BLE 广播 + buzz_cmds → display/LVGL/UI/按键/录音控制 → 表盘时钟服务（1Hz）
// → AXP2101 电源轮询（1Hz，须在 UI 后）→ REPL。
// 各阶段失败降级：音频失败 rec 不可用；BLE 失败仅失能同步；RTC 失败 TIME_SET 报 4999；
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
#include "sys/beep.h"
#include "sys/settings.h"
#include "sys/power_axp.h"
#include "sys/rtc_pcf.h"
#include "sys/settings_esp.h"
#include "sys/sync_marks_esp.h"
#include "ui/screen_pwr.h"
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

    // M5.4：设置框架 + 已同步标记（NVS 持久化）；应用动作在各子系统就绪后执行
    if (settings_esp_init() != ESP_OK) {
        ESP_LOGE(TAG, "settings init failed, defaults in use");
    }
    if (sync_marks_esp_init() != ESP_OK) {
        ESP_LOGE(TAG, "sync marks init failed, runtime-only");
    }

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

    // M5.4：设置应用（持久化值 → 各子系统；亮度无应用路径仅存值，见 M5_PLAN §9）
    {
        const uint8_t q = settings_get_u8(SETTINGS_REC_QUALITY);
        buzz_recorder_set_bitrate(q == 1u ? 24000 : 0);
        (void)buzz_audio_set_in_gain(settings_get_float(SETTINGS_MIC_GAIN_DB));
        beep_set_enabled(settings_get_u8(SETTINGS_BEEP_ENABLED) != 0u);
        screen_pwr_set_timeout_ms((uint32_t)settings_get_u8(SETTINGS_SCREEN_OFF_S) *
                                  1000u);
        settings_esp_dump();
    }

    // M5.4 三阶段：PCF85063 RTC 须在 BLE 之前就绪——否则客户端连接窗口内
    // TIME_SET 必然失败（bsp i2c 已由音频初始化链就绪；UI 不是 RTC 前置）。
    if (rtc_pcf_init() != ESP_OK) {
        ESP_LOGE(TAG, "rtc init failed, TIME_SET unavailable");
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

    // M5.4 收尾：表盘时钟服务（1Hz 读 RTC 维护快照；rtc 本体已在 BLE 前就绪）
    if (rtc_pcf_clock_service_start() != ESP_OK) {
        ESP_LOGE(TAG, "clock service start failed, watchface clock unavailable");
    }

    // M5.4 二阶段：AXP2101 电源轮询（须在 bsp i2c 与 UI 消息队列就绪后）
    if (power_axp_start() != ESP_OK) {
        ESP_LOGE(TAG, "power axp start failed, battery status unavailable");
    }

    if (console_cmd_start_repl() != ESP_OK) {
        ESP_LOGE(TAG, "console REPL start failed");
    }
}
