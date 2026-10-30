// Beecord USB 串口 REPL 实现（M2）：rec start/stop、ls 命令与 REPL 启动。
#include "console_cmd.h"

#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio/buzz_audio.h"
#include "audio/buzz_recorder.h"
#include "audio/rec_pipe.h"
#include "ble/buzz_ble.h"
#include "sys/keyd_gpio.h"
#include "sys/power_axp.h"
#include "sys/rtc.h"
#include "sys/rtc_pcf.h"
#include "sys/settings.h"

static const char *TAG = "console";

static bool s_repl_started;

// ---------- rec ----------

static void print_rec_usage(void)
{
    printf("Usage:\n  rec start [ogg|wav]  开始录音（缺省 ogg）\n  rec stop   停止录音\n");
}

static int cmd_rec(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "start") != 0 && strcmp(argv[1], "stop") != 0)) {
        print_rec_usage();
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        buzz_rec_fmt_t fmt = BUZZ_REC_FMT_OGG;
        if (argc >= 3) {
            if (strcasecmp(argv[2], "ogg") == 0) {
                fmt = BUZZ_REC_FMT_OGG;
            } else if (strcasecmp(argv[2], "wav") == 0) {
                fmt = BUZZ_REC_FMT_WAV;
            } else {
                printf("未知格式: %s\n", argv[2]);
                print_rec_usage();
                return 1;
            }
        }
        if (buzz_recorder_state() == BUZZ_REC_STATE_RECORDING) {
            printf("已在录音中，先 rec stop 再 start\n");
            return 1;
        }
        esp_err_t ret = buzz_recorder_start(fmt);
        if (ret != ESP_OK) {
            printf("rec start 失败: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("录音中\n");
        return 0;
    }

    // rec stop
    if (buzz_recorder_state() != BUZZ_REC_STATE_RECORDING) {
        printf("当前没有进行中的录音\n");
        return 1;
    }
    esp_err_t ret = buzz_recorder_stop();
    if (ret != ESP_OK) {
        printf("rec stop 失败: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("已停止\n");
    return 0;
}

// ---------- ls ----------

static int cmd_ls(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        printf("无法打开 %s（TF 卡未挂载？）\n", BSP_SD_MOUNT_POINT);
        return 1;
    }
    printf("%s:\n", BSP_SD_MOUNT_POINT);
    uint32_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char full[280]; // 挂载点 + '/' + NAME_MAX(255) + NUL，避免 -Wformat-truncation
        snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, ent->d_name);
        struct stat st;
        uint32_t size = 0;
        if (stat(full, &st) == 0) {
            size = (uint32_t)st.st_size;
        }
        printf("  %-32s %8" PRIu32 "\n", ent->d_name, size);
        count++;
    }
    closedir(dir);
    if (count == 0) {
        printf("  (empty)\n");
    }

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        printf("%" PRIu32 " 个文件，共 %" PRIu64 " MB / 剩余 %" PRIu64 " MB\n", count,
               total_bytes / (1024u * 1024u), free_bytes / (1024u * 1024u));
    }
    return 0;
}

// ---------- gain（运行期调麦克风增益） ----------

static int cmd_gain(int argc, char **argv)
{
    if (argc < 2) {
        printf("当前增益: %.1f dB\n",
               (double)settings_get_float(SETTINGS_MIC_GAIN_DB));
        printf("Usage:\n  gain <dB>  设置麦克风增益（0~37.5，3dB 步进就近取档，持久化）\n");
        return 1;
    }
    char *end = NULL;
    float db = strtof(argv[1], &end);
    if (end == argv[1]) {
        printf("参数须为数字\n");
        return 1;
    }
    esp_err_t ret = buzz_audio_set_in_gain(db);
    if (ret != ESP_OK) {
        printf("gain 设置失败: %s\n", esp_err_to_name(ret));
        return 1;
    }
    settings_set_float(SETTINGS_MIC_GAIN_DB, db); // M5.4：持久化（写失败仅告警）
    if (!settings_last_save_ok()) {
        printf("警告: 增益持久化失败（运行值已生效；重启回上次成功保存值）\n");
    }
    printf("增益已设为 %.1f dB\n", (double)db);
    return 0;
}

// ---------- get（base64 串口导出，免读卡器取文件） ----------

// 行宽 76 字符的标准 base64 分块：57 字节原始数据 → 76 字符一行。
// 输出经 vfs 会做 \n→\r\n 转换，base64 字母表不受影响，宿主端剥离空白即可
#define GET_CHUNK_RAW 57u
#define GET_B64_LINE 76u

static const char s_b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// in[0..len) → out[0..4*ceil(len/3))，标准 base64（含 '=' 填充），不附加 NUL
static void b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (rem > 2) {
            v |= in[i + 2];
        }
        out[o++] = s_b64_tab[(v >> 18) & 0x3f];
        out[o++] = s_b64_tab[(v >> 12) & 0x3f];
        out[o++] = rem > 1 ? s_b64_tab[(v >> 6) & 0x3f] : '=';
        out[o++] = rem > 2 ? s_b64_tab[v & 0x3f] : '=';
    }
}

static int cmd_get(int argc, char **argv)
{
    if (argc < 2 || strchr(argv[1], '/') != NULL || strchr(argv[1], '.') == NULL) {
        printf("Usage:\n  get <文件名> [最大字节数]  base64 导出 /sdcard 根目录文件\n");
        return 1;
    }
    long max_bytes = LONG_MAX;
    if (argc >= 3) {
        char *end = NULL;
        max_bytes = strtol(argv[2], &end, 10);
        if (end == argv[2] || max_bytes <= 0) {
            printf("最大字节数须为正整数\n");
            return 1;
        }
    }
    char full[280];
    snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, argv[1]);
    struct stat st;
    if (stat(full, &st) != 0) {
        printf("文件不存在: %s\n", full);
        return 1;
    }
    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        printf("无法打开: %s\n", full);
        return 1;
    }
    long to_send = (long)st.st_size < max_bytes ? (long)st.st_size : max_bytes;
    printf("-----BEGIN %s size=%ld send=%ld-----\n", argv[1], (long)st.st_size, to_send);
    fflush(stdout);

    static uint8_t raw[GET_CHUNK_RAW];   // 静态缓冲：不占 REPL 任务栈
    static uint8_t b64[GET_B64_LINE];
    long sent = 0;
    while (sent < to_send) {
        size_t want = (size_t)(to_send - sent);
        if (want > sizeof(raw)) {
            want = sizeof(raw);
        }
        size_t n = fread(raw, 1, want, f);
        if (n == 0) {
            break;
        }
        b64_encode(raw, n, (char *)b64);
        size_t olen = 4 * ((n + 2) / 3);
        fwrite(b64, 1, olen, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        sent += (long)n;
    }
    fclose(f);
    printf("-----END %s sent=%ld-----\n", argv[1], sent);
    fflush(stdout);
    return sent == to_send ? 0 : 1;
}

// ---------- ble（BLE 状态查询） ----------

static int cmd_ble(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("device:   %s\n", buzz_ble_device_name());
    printf("connected: %s\n", buzz_ble_is_connected() ? "yes" : "no");
    if (buzz_ble_is_connected()) {
        printf("conn_handle: %u\n", (unsigned)buzz_ble_conn_handle());
    }
    printf("mtu:      %u\n", (unsigned)buzz_ble_mtu());
    printf("notify subscribed: %s\n", buzz_ble_notify_subscribed() ? "yes" : "no");
    return 0;
}

static int cmd_heap(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("internal free: %" PRIu32 " B\n", esp_get_free_internal_heap_size());
    printf("total free:    %" PRIu32 " B (含 PSRAM)\n", esp_get_free_heap_size());
    printf("min internal:  %" PRIu32 " B\n", esp_get_minimum_free_heap_size());
    return 0;
}

// ---------- i2cscan（M5.2：PWR 键扩展器候选探测） ----------

// 共享总线已知设备（HARDWARE.md）：触控 FT5x06、AXP2101、RTC PCF85063、
// ES8311、ES7210、IMU；扫出表外未知地址即 PWR 扩展器候选。
static const struct {
    uint8_t addr;
    const char *name;
} s_i2c_known[] = {
    {0x18, "ES8311?"}, // 0x18/0x30 为 ES8311 地址绑带两档（0x18 实测应答、0x30 无）
    {0x30, "ES8311"},
    {0x34, "AXP2101"},
    {0x38, "FT5x06 触控"},
    {0x40, "ES7210"},
    {0x51, "PCF85063 RTC"},
    {0x6A, "QMI8658C IMU?"},
    {0x6B, "QMI8658C IMU?"},
};

static int cmd_i2cscan(int argc, char **argv)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        printf("I2C 总线未初始化\n");
        return 1;
    }
    // 单地址调试模式：`i2cscan <addr10>` 探单个地址并回显精确错误码
    // （区分 NACK=未应答 / TIMEOUT=总线被拉死；PWR 验证排查用）
    if (argc >= 2) {
        int addr = atoi(argv[1]);
        if (addr < 0x08 || addr > 0x77) {
            printf("地址须为 0x08~0x77 的十进制数\n");
            return 1;
        }
        esp_err_t ret = i2c_master_probe(bus, (uint8_t)addr, pdMS_TO_TICKS(200));
        printf("probe 0x%02X → %s\n", addr, esp_err_to_name(ret));
        return 0;
    }
    // i2c_master_probe 对每个无应答地址都会打一条 timeout 错误日志（刷屏），
    // 扫描期间临时静音该组件日志，扫完恢复默认级别
    esp_log_level_t saved = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    printf("扫描共享 I2C 总线 (0x08-0x77)...\n");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, pdMS_TO_TICKS(200)) == ESP_OK) {
            const char *name = NULL;
            for (size_t i = 0; i < sizeof(s_i2c_known) / sizeof(s_i2c_known[0]); i++) {
                if (s_i2c_known[i].addr == addr) {
                    name = s_i2c_known[i].name;
                    break;
                }
            }
            printf("  0x%02X  %s%s\n", addr, name != NULL ? name : "未知设备",
                   name != NULL ? "" : "  <-- 扩展器候选（PWR 检测备选路径）");
            found++;
        }
    }
    esp_log_level_set("i2c.master", saved);
    printf("共 %d 个应答地址\n", found);
    return 0;
}

// ---------- keylvl（M5.2：PWR 键 GPIO10 验证观测） ----------

static int cmd_keylvl(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    keyd_gpio_dump_levels();
    return 0;
}

// ---------- quality（录音质量两档；M5.4 起 NVS 持久化，设置页 UI 归 M5.5） ----------

static int cmd_quality(int argc, char **argv)
{
    if (argc < 2) {
        printf("当前: %s（%d bps）\n",
               buzz_recorder_get_bitrate() == 0 ? "标准" : "省空间",
               buzz_recorder_get_bitrate() == 0 ? BUZZ_REC_PIPE_BITRATE
                                                : buzz_recorder_get_bitrate());
        printf("Usage:\n  quality 0  标准 ~44kbps\n  quality 1  省空间 ~24kbps\n");
        return 1;
    }
    int q = atoi(argv[1]);
    if (q != 0 && q != 1) {
        printf("仅支持 0（标准）/ 1（省空间）\n");
        return 1;
    }
    buzz_recorder_set_bitrate(q == 0 ? 0 : 24000);
    settings_set_u8(SETTINGS_REC_QUALITY, (uint8_t)q); // M5.4：持久化
    if (!settings_last_save_ok()) {
        printf("警告: 质量档持久化失败（运行值已生效；重启回上次成功保存值）\n");
    }
    printf("已设置：%s（下次录音生效，已持久化）\n",
           q == 0 ? "标准 ~44kbps" : "省空间 ~24kbps");
    return 0;
}

// M5.4：设置项总览（亮度为持久化值，当前无应用路径——背光命令与 LVGL flush
// 并发有卡死前科，M5_PLAN §9 登记，不接应用）
static int cmd_settings(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rec_quality : %u（0 标准44k / 1 省空间24k）\n",
           (unsigned)settings_get_u8(SETTINGS_REC_QUALITY));
    printf("mic_gain    : %.1f dB\n", (double)settings_get_float(SETTINGS_MIC_GAIN_DB));
    printf("beep        : %s\n",
           settings_get_u8(SETTINGS_BEEP_ENABLED) != 0u ? "on" : "off");
    printf("brightness  : %u（仅存值未应用）\n",
           (unsigned)settings_get_u8(SETTINGS_BRIGHTNESS));
    printf("screen_off  : %u s（0=永不）\n",
           (unsigned)settings_get_u8(SETTINGS_SCREEN_OFF_S));
    printf("last save   : %s\n", settings_last_save_ok() ? "ok" : "FAILED");
    return 0;
}

// ---------- reclvl（M5.3：录音实时统计观测，电平条排查用） ----------

static int cmd_reclvl(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("state:    %d (0=idle 1=recording 2=paused)\n",
           (int)buzz_recorder_state());
    printf("elapsed:  %" PRIu32 " ms\n", buzz_recorder_elapsed_ms());
    printf("level:    %u / 24\n", (unsigned)buzz_recorder_level());
    printf("bytes:    %" PRIu64 "\n", buzz_recorder_bytes_written());
    printf("marks:    %" PRIu32 "\n", buzz_recorder_mark_count());
    printf("path:     %s\n", buzz_recorder_current_path());
    uint32_t rms_last = 0, rms_min = 0, rms_max = 0;
    buzz_recorder_meter_diag(&rms_last, &rms_min, &rms_max);
    printf("raw rms:  last=%" PRIu32 " min=%" PRIu32 " max=%" PRIu32
           "（满度 32767；3277=-20dB）\n",
           rms_last, rms_min, rms_max);
    return 0;
}

// ---------- pwr（M5.4：电源状态查询） ----------

static int cmd_pwr(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    float vbat_mv = 0.0f;
    uint8_t raw_pct = 0;
    uint8_t smoothed_pct = 0xFFu;
    bool charging = false;
    uint32_t ok = 0;
    uint32_t errs = 0;
    power_axp_get_debug(&vbat_mv, &raw_pct, &smoothed_pct, &charging, &ok, &errs);
    printf("vbat:     %.0f mV\n", (double)vbat_mv);
    printf("raw pct:  %u%%\n", (unsigned)raw_pct);
    printf("smoothed: %s\n",
           smoothed_pct == 0xFFu ? "unknown" : "ok");
    if (smoothed_pct != 0xFFu) {
        printf("smoothed: %u%%\n", (unsigned)smoothed_pct);
    }
    printf("charging: %s\n", charging ? "yes" : "no");
    printf("reads:    %" PRIu32 " ok / %" PRIu32 " err\n", ok, errs);
    return 0;
}

// ---------- rtc（M5.4 三阶段：RTC 状态查询 / 调试设置；正常验收走 BLE TIME_SET） ----------

static int cmd_rtc(int argc, char **argv)
{
    // rtc set <epochMs> <utcOffsetMin>：严格解析与范围校验（同 §8.4）
    if (argc >= 2 && strcmp(argv[1], "set") == 0) {
        if (argc < 4) {
            printf("用法: rtc set <epochMs> <utcOffsetMin>\n");
            return 1;
        }
        char *end = NULL;
        const long long epoch_ms = strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            printf("epochMs 须为整数\n");
            return 1;
        }
        const long off = strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0') {
            printf("utcOffsetMin 须为整数\n");
            return 1;
        }
        rtc_datetime_t local;
        if (!rtc_time_set_convert((int64_t)epoch_ms, (int)off, &local)) {
            printf("越界：epochMs ∈ [1704067200000, 4102444799999]，"
                   "utcOffsetMin ∈ [-720, 840]\n");
            return 1;
        }
        if (rtc_pcf_write(&local) != ESP_OK) {
            printf("RTC 写入失败\n");
            return 1;
        }
        printf("已设置本地时间 %04u-%02u-%02u %02u:%02u:%02u（UTC%+ldmin）\n",
               (unsigned)local.year, (unsigned)local.month, (unsigned)local.day,
               (unsigned)local.hour, (unsigned)local.minute, (unsigned)local.second,
               off);
        return 0;
    }

    // rtc：查询
    rtc_datetime_t dt;
    bool os_stopped = false;
    esp_err_t ret = rtc_pcf_read(&dt, &os_stopped);
    if (ret != ESP_OK) {
        printf("RTC 无效：%s（OS=%d）——未对时或振荡器停过，不当真时间\n",
               ret == ESP_ERR_INVALID_RESPONSE ? "时钟完整性不足" : "读取失败",
               (int)os_stopped);
        return 0;
    }
    printf("本地时间: %04u-%02u-%02u %02u:%02u:%02u（UTC+offset）\n",
           (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
           (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    printf("OS=%d（0=时钟完整）  weekday=%u\n", (int)os_stopped,
           (unsigned)rtc_weekday(&dt));
    return 0;
}

// ---------- REPL 启动 ----------

esp_err_t console_cmd_start_repl(void)
{
    if (s_repl_started) {
        return ESP_ERR_INVALID_STATE;
    }

    // 注意：esp_console_init 由 new_repl 内部的 esp_console_common_init 完成，
    // 不能自己先调（重复初始化返回 ESP_ERR_INVALID_STATE 导致 repl create failed）
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "beecord> ";
    // M5.1：`ui snap` 的 LVGL 快照渲染（lv_draw_dispatch）在本任务上下文执行，
    // 默认 4KB 栈溢出（实测 stack overflow in task console_repl）→ 16KB
    repl_cfg.task_stack_size = 16384;
    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl),
                        TAG, "repl create failed");

    const esp_console_cmd_t rec_cmd = {
        .command = "rec",
        .help = "录音控制",
        .hint = "start [ogg|wav]|stop",
        .func = &cmd_rec,
    };
    const esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help = "列出 TF 卡根目录文件",
        .hint = NULL,
        .func = &cmd_ls,
    };
    const esp_console_cmd_t get_cmd = {
        .command = "get",
        .help = "base64 导出 TF 卡文件（串口传输）",
        .hint = "<文件名> [最大字节数]",
        .func = &cmd_get,
    };
    const esp_console_cmd_t gain_cmd = {
        .command = "gain",
        .help = "设置麦克风增益 dB（0~37.5）",
        .hint = "<dB>",
        .func = &cmd_gain,
    };
    const esp_console_cmd_t ble_cmd = {
        .command = "ble",
        .help = "BLE 状态（设备名/连接/MTU/订阅）",
        .hint = NULL,
        .func = &cmd_ble,
    };
    const esp_console_cmd_t heap_cmd = {
        .command = "heap",
        .help = "堆余量（内部 RAM / 含 PSRAM / 历史最小内部）",
        .hint = NULL,
        .func = &cmd_heap,
    };
    const esp_console_cmd_t i2cscan_cmd = {
        .command = "i2cscan",
        .help = "扫描共享 I2C 总线未知地址（PWR 扩展器候选探测）",
        .hint = NULL,
        .func = &cmd_i2cscan,
    };
    const esp_console_cmd_t keylvl_cmd = {
        .command = "keylvl",
        .help = "打印 BOOT/PWR 键当前电平（PWR 检测验证用）",
        .hint = NULL,
        .func = &cmd_keylvl,
    };
    const esp_console_cmd_t settings_cmd = {
        .command = "settings",
        .help = "设置项总览（NVS 持久化：质量/增益/提示音/亮度/熄屏时间）",
        .hint = NULL,
        .func = &cmd_settings,
    };
    const esp_console_cmd_t rtc_cmd = {
        .command = "rtc",
        .help = "RTC 状态查询 / rtc set <epochMs> <utcOffsetMin> 调试设置",
        .hint = "[set <epochMs> <offsetMin>]",
        .func = &cmd_rtc,
    };
    const esp_console_cmd_t pwr_cmd = {
        .command = "pwr",
        .help = "电源状态（电压/原始与平滑百分比/充电态/读取健康）",
        .hint = NULL,
        .func = &cmd_pwr,
    };
    const esp_console_cmd_t reclvl_cmd = {
        .command = "reclvl",
        .help = "录音实时统计（状态/时长/电平/字节/标记），电平条排查用",
        .hint = NULL,
        .func = &cmd_reclvl,
    };
    const esp_console_cmd_t quality_cmd = {
        .command = "quality",
        .help = "录音质量两档（0 标准 44kbps / 1 省空间 24kbps，下次录音生效）",
        .hint = "0|1",
        .func = &cmd_quality,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&rec_cmd), TAG, "register rec failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ls_cmd), TAG, "register ls failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&get_cmd), TAG, "register get failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&gain_cmd), TAG, "register gain failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ble_cmd), TAG, "register ble failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&heap_cmd), TAG, "register heap failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&i2cscan_cmd), TAG, "register i2cscan failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&keylvl_cmd), TAG, "register keylvl failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&quality_cmd), TAG, "register quality failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reclvl_cmd), TAG, "register reclvl failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&settings_cmd), TAG, "register settings failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&pwr_cmd), TAG, "register pwr failed");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&rtc_cmd), TAG, "register rtc failed");

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "repl start failed");

    s_repl_started = true;
    ESP_LOGI(TAG, "REPL ready (USB Serial/JTAG), try 'rec start [ogg|wav]' / 'rec stop' / 'ls'");
    return ESP_OK;
}
