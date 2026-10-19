// Beecord 音频采集封装实现（M2）。初始化序列与官方 05_Spec_Analyzer 的
// bsp_extra_codec_set_fs 同路径，参数取 16kHz/16bit/2ch/33dB（增益为 M2.4 实测定案）。
#include "buzz_audio.h"

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"

static const char *TAG = "buzz_audio";

// open 成功后缓存的采集句柄，供运行期调增益（REPL gain 命令）使用
static esp_codec_dev_handle_t s_mic_handle;

esp_err_t buzz_audio_open_mic(esp_codec_dev_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // BSP 内部幂等：首次调用时自行完成 bsp_i2c_init + bsp_audio_init（I2S tx/rx 创建）
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (mic == NULL) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BUZZ_AUDIO_SAMPLE_RATE,
        .channel = BUZZ_AUDIO_CHANNELS,
        .bits_per_sample = BUZZ_AUDIO_BITS,
        // channel_mask = 0：不过滤通道（双麦全收，软件混单声道）
    };
    int ret = esp_codec_dev_open(mic, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        return ESP_FAIL;
    }

    ret = esp_codec_dev_set_in_gain(mic, BUZZ_AUDIO_IN_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_set_in_gain failed: %d", ret);
        esp_codec_dev_close(mic);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "mic ready: %uHz/%ubit/%uch gain=%.1fdB",
             BUZZ_AUDIO_SAMPLE_RATE, BUZZ_AUDIO_BITS, BUZZ_AUDIO_CHANNELS,
             (double)BUZZ_AUDIO_IN_GAIN_DB);
    s_mic_handle = mic;
    *out_handle = mic;
    return ESP_OK;
}

esp_err_t buzz_audio_set_in_gain(float db)
{
    if (s_mic_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // ES7210 PGA 有效范围 0~37.5dB（3dB 步进档位，驱动内部就近取档）
    if (db < 0.0f || db > 37.5f) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_set_in_gain(s_mic_handle, db) == ESP_CODEC_DEV_OK ? ESP_OK
                                                                           : ESP_FAIL;
}

esp_err_t buzz_audio_close_mic(esp_codec_dev_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_close(handle) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t buzz_audio_deinit_mic(esp_codec_dev_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_codec_dev_close(handle);
    esp_codec_dev_delete(handle);
    if (s_mic_handle == handle) {
        s_mic_handle = NULL;
    }
    return ESP_OK;
}
