// Beecord 录音 journal 目标侧文件胶实现（rec_journal_esp.c，M5.3）。
#include "rec_journal_esp.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "recjrnl";
#define JOURNAL_PATH BSP_SD_MOUNT_POINT "/RECJRNL"

esp_err_t rec_journal_save(const rec_journal_t *j)
{
    if (j == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    int len = rec_journal_encode(j, buf, sizeof(buf));
    if (len < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // 先写后刷：journal 短暂超前/落后于真实数据均可容忍（截断目标取较小侧）
    FILE *fp = fopen(JOURNAL_PATH, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    bool ok = fwrite(buf, 1, (size_t)len, fp) == (size_t)len;
    ok = (fflush(fp) == 0) && ok;
    if (fclose(fp) != 0) {
        ok = false;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t rec_journal_clear(void)
{
    if (unlink(JOURNAL_PATH) != 0) {
        return ESP_FAIL; // 不存在亦返回失败，调用方按需忽略
    }
    return ESP_OK;
}

esp_err_t rec_journal_check_recovery(char *out_recovered_file, size_t file_cap,
                                     bool *recovered)
{
    if (recovered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *recovered = false;
    FILE *fp = fopen(JOURNAL_PATH, "rb");
    if (fp == NULL) {
        return ESP_OK; // 无残留：正常路径
    }

    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    bool extra = fgetc(fp) != EOF; // 定长文件之外还有内容 = 异常
    fclose(fp);
    if (n != sizeof(buf) || extra) {
        ESP_LOGE(TAG, "journal 长度异常（%u 字节）", (unsigned)n);
        return ESP_FAIL; // 保留现场待排查
    }

    rec_journal_t j;
    if (rec_journal_decode(buf, sizeof(buf), &j) != 0) {
        ESP_LOGE(TAG, "journal 校验失败（magic/ver/crc）");
        return ESP_FAIL; // 保留现场待排查
    }

    // 组装完整路径并取实际大小
    char path[REC_JOURNAL_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", BSP_SD_MOUNT_POINT, j.file);
    uint64_t actual_size = 0;
    struct stat st;
    if (stat(path, &st) == 0) {
        actual_size = (uint64_t)st.st_size;
    }

    uint64_t truncate_to = 0;
    if (!rec_journal_plan_recovery(&j, actual_size, &truncate_to)) {
        ESP_LOGW(TAG, "journal 指向的文件不存在：%s（仅提示，不动文件）", path);
        // 文件没了：journal 无意义，清掉，返回已恢复=false + 特殊说明走日志
        (void)rec_journal_clear();
        return ESP_OK;
    }

    // 截断到已确认字节（断电撕毁的尾页丢弃）
    if (actual_size > truncate_to) {
        int fd = open(path, O_WRONLY);
        if (fd < 0 || ftruncate(fd, (off_t)truncate_to) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            ESP_LOGE(TAG, "truncate %s → %" PRIu64 " failed", path, truncate_to);
            return ESP_FAIL; // 保留 journal 待重试/排查
        }
        close(fd);
        ESP_LOGI(TAG, "recovered: %s truncated to %" PRIu64 " bytes", path,
                 truncate_to);
    }

    // WAV：回填头长度（头 44 字节在文件头，直接读改写）
    if (j.fmt == 1u) {
        FILE *wfp = fopen(path, "r+");
        if (wfp != NULL) {
            uint8_t hdr[44];
            if (fread(hdr, 1, sizeof(hdr), wfp) == sizeof(hdr)) {
                rec_journal_wav_patch_header(hdr, truncate_to - sizeof(hdr));
                rewind(wfp);
                (void)fwrite(hdr, 1, sizeof(hdr), wfp);
                (void)fflush(wfp);
            }
            fclose(wfp);
        }
    }

    if (out_recovered_file != NULL && file_cap > 0) {
        snprintf(out_recovered_file, file_cap, "%s", j.file);
    }
    (void)rec_journal_clear();
    *recovered = true;
    ESP_LOGI(TAG, "recovered session: %s（marks=%" PRIu32 " dur=%" PRIu32 "ms）", j.file,
             j.marks, j.elapsed_ms);
    return ESP_OK;
}
