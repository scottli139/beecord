// Beecord 固件入口（M0 骨架 + M1 自检探针 + M2 音频采集与 REPL 录控）。
// 启动序列：版本打印 → TF 卡探测挂载 → 麦克风初始化（16kHz/16bit/双麦）→
// 采集任务创建 → USB 串口 REPL（rec start/stop、ls）。
// 各阶段失败降级：音频失败则 rec 命令提示不可用，REPL 始终启动保证可交互。
#include "buzz_audio.h"
#include "buzz_recorder.h"
#include "buzz_sdcard.h"
#include "console_cmd.h"
#include "esp_log.h"

static const char *TAG = "beecord";

#define FW_VERSION "0.3.0-m3"

void app_main(void)
{
    ESP_LOGI(TAG, "Beecord firmware %s", FW_VERSION);
    buzz_sdcard_probe();

    esp_codec_dev_handle_t mic = NULL;
    if (buzz_audio_open_mic(&mic) != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed, rec unavailable");
    } else if (buzz_recorder_init(mic) != ESP_OK) {
        ESP_LOGE(TAG, "recorder init failed, rec unavailable");
    }

    if (console_cmd_start_repl() != ESP_OK) {
        ESP_LOGE(TAG, "console REPL start failed");
    }
}
