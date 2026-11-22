#pragma once

#include "esp_err.h"
#include "rec_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在 TF 卡根目录写入并同步音频对应的 sidecar；任一步失败返回非 ESP_OK。
esp_err_t rec_meta_save(const char *audio_name, const rec_meta_t *meta);

// 删除音频对应的 sidecar；不存在视为成功。
esp_err_t rec_meta_remove(const char *audio_name);

#ifdef __cplusplus
}
#endif
