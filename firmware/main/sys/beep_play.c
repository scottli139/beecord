// Beecord 提示音 ESP 侧播放实现（beep_play.c，M5.2）。
// 16kHz/16bit 单声道正弦 + 5ms 线性起止包络（防爆音）。
//
// 播放时序（M5.2 实机听辨失败后的定案，两次实验实锤）：
// - I2S DMA 缓冲深度 ~200ms：esp_codec_dev_write 只把数据顶进 DMA，立即 close
//   会把未播出的部分全部截掉（80ms 短音全灭、300ms 长音只剩 ~100ms 碎片）。
//   故收尾再写 250ms 静音——write 的阻塞特性会把真实音频全部顶出去播完。
// - ES8311 模拟上电/解静音有爬坡：open 后紧跟的短音会被吞，前导先写 100ms 静音。
#include "beep_play.h"

#include <math.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// M_PI 在严格 ANSI 下不可见（newlib），自定义常量稳妥
#define BEEP_PI 3.14159265358979323846

static const char *TAG = "beep";

#define BEEP_SAMPLE_RATE 16000u
#define BEEP_CHANNELS 1u
#define BEEP_BITS 16u
#define BEEP_ENV_MS 5u            // 起止包缘时长（防爆音）
#define BEEP_OUT_VOL 60           // 默认扬声器音量 0~100（听辨后用户觉得轻再调）
#define BEEP_LEAD_IN_MS 100u      // 前导静音：等 ES8311 模拟爬坡过去（M5.2 实锤）
#define BEEP_TAIL_MS 250u         // 收尾静音：把 DMA 里未播出的真实音频顶完（M5.2 实锤）
// 缓冲按调试上限 2000ms 音长分配（64KB PSRAM；事件音最长仅 300ms）——
// 曾按事件最长 420ms 分配，beep t 1000（1s 音）越界写堆 → queue assert 实锤
#define BEEP_TONE_MAX_SAMPLES (2000u * BEEP_SAMPLE_RATE / 1000)

// 单音 PCM 缓冲（按 2000ms 上限 64KB；静态内部 RAM 会把 LVGL DMA 行缓冲挤出
// 分配失败——M5.2 实机实锤，改 PSRAM 惰性分配）
static int16_t *s_pcm;

static int16_t *beep_pcm_buf(void)
{
    if (s_pcm == NULL) {
        s_pcm = heap_caps_malloc(BEEP_TONE_MAX_SAMPLES * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_pcm;
}

// 生成单个正音：freq 正弦 + 线性起止包络
static size_t synth_tone(uint16_t freq_hz, uint16_t dur_ms)
{
    size_t n = (size_t)dur_ms * BEEP_SAMPLE_RATE / 1000;
    size_t env = (size_t)BEEP_ENV_MS * BEEP_SAMPLE_RATE / 1000;
    if (env * 2 > n) {
        env = n / 4; // 极短音（40ms）包络收窄
    }
    for (size_t i = 0; i < n; i++) {
        double ph = 2.0 * BEEP_PI * (double)freq_hz * (double)i / (double)BEEP_SAMPLE_RATE;
        double amp = 0.6; // 振幅余量（防爆音）
        if (i < env) {
            amp *= (double)i / (double)env;
        } else if (i >= n - env) {
            amp *= (double)(n - 1 - i) / (double)env;
        }
        s_pcm[i] = (int16_t)(32767.0 * amp * sin(ph));
    }
    return n;
}

// 写一段静音（前导/收尾/音间共用）
static esp_err_t write_silence(esp_codec_dev_handle_t spk, uint16_t dur_ms)
{
    size_t bytes = (size_t)dur_ms * BEEP_SAMPLE_RATE / 1000 * sizeof(int16_t);
    memset(s_pcm, 0, bytes);
    return esp_codec_dev_write(spk, s_pcm, bytes) == ESP_CODEC_DEV_OK ? ESP_OK
                                                                      : ESP_FAIL;
}

// 播放核心：按序列播音（前导静音 → 音+间隔 → 收尾静音 → close）。
// vol 0~100 传入；静音段不计入 beep.c 的 beep_sequence_duration_ms 预算
// （该函数只算音长+间隔，注释已说明）。
static esp_err_t play_tones(const beep_tone_t *tones, uint8_t tone_count, int vol)
{
    if (beep_pcm_buf() == NULL) {
        ESP_LOGE(TAG, "pcm buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(spk != NULL, ESP_FAIL, TAG, "speaker init failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = BEEP_SAMPLE_RATE,
        .channel = BEEP_CHANNELS,
        .bits_per_sample = BEEP_BITS,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "speaker open failed");
    esp_codec_dev_set_out_vol(spk, vol);

    // 前导静音：ES8311 上电/解静音爬坡会吞紧跟的短音（M5.2 实锤）
    ESP_RETURN_ON_ERROR(write_silence(spk, BEEP_LEAD_IN_MS), TAG, "lead-in write failed");

    for (uint8_t i = 0; i < tone_count; i++) {
        size_t n = synth_tone(tones[i].freq_hz, tones[i].dur_ms);
        ESP_RETURN_ON_FALSE(esp_codec_dev_write(spk, s_pcm, n * sizeof(int16_t)) ==
                                ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "speaker write failed");
        if (i + 1 < tone_count) {
            ESP_RETURN_ON_ERROR(write_silence(spk, BEEP_GAP_MS), TAG, "gap write failed");
        }
    }

    // 收尾静音：write 阻塞把 DMA 里未播出的真实音频全部顶出去，防 close 截断
    ESP_RETURN_ON_ERROR(write_silence(spk, BEEP_TAIL_MS), TAG, "tail write failed");
    esp_codec_dev_close(spk);
    ESP_LOGI(TAG, "beep done (tones=%u vol=%d)", (unsigned)tone_count, vol);
    return ESP_OK;
}

esp_err_t beep_play(beep_event_t evt)
{
    if (!beep_is_enabled()) {
        return ESP_OK; // 全局关闭：静默（设置项 M5.4 接 NVS）
    }
    uint8_t tone_count = 0;
    const beep_tone_t *tones = beep_get_sequence(evt, &tone_count);
    ESP_RETURN_ON_FALSE(tones != NULL && tone_count > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid beep event %d", (int)evt);
    return play_tones(tones, tone_count, BEEP_OUT_VOL);
}

esp_err_t beep_play_tone(uint16_t freq_hz, uint16_t dur_ms, int vol)
{
    if (!beep_is_enabled()) {
        return ESP_OK;
    }
    beep_tone_t t = {freq_hz, dur_ms};
    return play_tones(&t, 1, vol);
}
