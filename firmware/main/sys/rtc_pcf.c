// Beecord PCF85063 RTC 实机适配实现（rtc_pcf.c，M5.4 三阶段）。
// 寄存器依据 NXP PCF85063A 数据手册：0x00 Control_1（bit5 STOP），
// 0x04..0x0A = Seconds..Years（BCD， Seconds bit7 = OS 标志），地址自动递增。
#include <inttypes.h>

#include "rtc_pcf.h"

#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rtc";
#define PCF_ADDR 0x51u

#define REG_CONTROL1 0x00u
#define REG_SECONDS 0x04u
#define C1_STOP (1u << 5)
#define SEC_OS (1u << 7)

static i2c_master_dev_handle_t s_dev;
static bool s_inited;

// 表盘时钟服务状态（M5.4 收尾）：低频任务写、UI 定时器读——瞬态字宽快照，
// 无需加锁（秒级时间显示对单次撕裂读不敏感）
static rtc_datetime_t s_clock_last;
static volatile bool s_clock_valid;
static uint8_t s_clock_fail_cnt;
static uint32_t s_clock_read_errs;

#define CLOCK_FAIL_INVALIDATE 10u // 连续失败该数 → 显示回"未对时"
#define CLOCK_FAIL_LOG_EVERY 32u
#define CLOCK_TASK_STACK 4096u // I2C + ESP 日志调用链；2048 实机触发栈保护

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, uint32_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t tx[2] = { reg, val };
    return i2c_master_transmit(s_dev, tx, sizeof(tx), pdMS_TO_TICKS(100));
}

esp_err_t rtc_pcf_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "bsp i2c not ready（须在音频/触控初始化之后）");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF_ADDR,
        .scl_speed_hz = 100000, // RTC 低频，共享总线友好
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG,
                        "rtc device add failed");
    s_inited = true;
    ESP_LOGI(TAG, "PCF85063 up (0x51)");
    return ESP_OK;
}

esp_err_t rtc_pcf_read(rtc_datetime_t *out, bool *os_stopped)
{
    ESP_RETURN_ON_FALSE(s_inited && out != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "rtc not init");
    uint8_t t[7] = { 0 };
    ESP_RETURN_ON_ERROR(read_regs(REG_SECONDS, t, sizeof(t)), TAG, "read time failed");

    out->second = rtc_bcd2bin(t[0] & (uint8_t)~SEC_OS);
    out->minute = rtc_bcd2bin(t[1]);
    out->hour = rtc_bcd2bin(t[2]);
    out->day = rtc_bcd2bin(t[3]);
    out->month = rtc_bcd2bin(t[5] & 0x1Fu); // 月寄存器高位未用
    out->year = (uint16_t)(2000u + rtc_bcd2bin(t[6]));
    const bool os = (t[0] & SEC_OS) != 0u;
    if (os_stopped != NULL) {
        *os_stopped = os;
    }
    // 垃圾态防御：OS 置位或日历非法 → 数据不可信（调用方按"无效"处理，
    // 绝不把 2000/1970 默认值当真时间）
    if (os || !rtc_datetime_valid(out)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

// 写时间主体（STOP 已置位后调用）
static esp_err_t write_time_regs(const rtc_datetime_t *local)
{
    uint8_t t[8]; // [reg_addr, sec..year]
    t[0] = REG_SECONDS;
    t[1] = rtc_bin2bcd(local->second); // OS 位写 0 = 清标志
    t[2] = rtc_bin2bcd(local->minute);
    t[3] = rtc_bin2bcd(local->hour);
    t[4] = rtc_bin2bcd(local->day);
    t[5] = rtc_weekday(local);
    t[6] = rtc_bin2bcd(local->month);
    t[7] = rtc_bin2bcd((uint8_t)(local->year - 2000u));
    return i2c_master_transmit(s_dev, t, sizeof(t), pdMS_TO_TICKS(100));
}

esp_err_t rtc_pcf_write(const rtc_datetime_t *local)
{
    ESP_RETURN_ON_FALSE(s_inited && local != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "rtc not init");
    ESP_RETURN_ON_FALSE(rtc_datetime_valid(local), ESP_ERR_INVALID_ARG, TAG,
                        "datetime out of range");

    // try/finally 语义：任何一步失败都【尽力恢复起振】（绝不留永久停振态），
    // 首要错误优先返回；OS/STOP 异常不得静默。
    uint8_t c1 = 0;
    esp_err_t first_err = ESP_OK;
    ESP_RETURN_ON_ERROR(read_regs(REG_CONTROL1, &c1, 1), TAG, "read control1");

    esp_err_t err = write_reg(REG_CONTROL1, (uint8_t)(c1 | C1_STOP));
    if (err == ESP_OK) {
        err = write_time_regs(local);
    }
    if (err != ESP_OK) {
        first_err = err;
        ESP_LOGE(TAG, "rtc write failed (%s)，尝试恢复起振", esp_err_to_name(err));
    }
    // 尽力恢复 STOP=0（用读到的原 control1）；恢复失败且此前无错则上抛恢复错误
    const esp_err_t resume_err = write_reg(REG_CONTROL1, (uint8_t)(c1 & (uint8_t)~C1_STOP));
    if (first_err == ESP_OK && resume_err != ESP_OK) {
        first_err = resume_err;
        ESP_LOGE(TAG, "rtc 起振恢复失败: %s", esp_err_to_name(resume_err));
    }
    if (first_err != ESP_OK) {
        return first_err;
    }

    // 读回校验（完整日历转秒后容忍 ±1s，覆盖 59→00 的分钟/日期进位）；读回失败含 OS 置位
    rtc_datetime_t back;
    bool os = false;
    err = rtc_pcf_read(&back, &os);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtc 读回校验失败: %s", esp_err_to_name(err));
        return err;
    }
    const int64_t actual_s = rtc_datetime_to_epoch(&back);
    const int64_t expected_s = rtc_datetime_to_epoch(local);
    const int64_t delta_s = actual_s - expected_s;
    if (delta_s < -1 || delta_s > 1) {
        ESP_LOGE(TAG, "rtc 读回校验不符：写 %04u-%02u-%02u %02u:%02u:%02u 读回 "
                      "%04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)local->year, (unsigned)local->month, (unsigned)local->day,
                 (unsigned)local->hour, (unsigned)local->minute, (unsigned)local->second,
                 (unsigned)back.year, (unsigned)back.month, (unsigned)back.day,
                 (unsigned)back.hour, (unsigned)back.minute, (unsigned)back.second);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "rtc set %04u-%02u-%02u %02u:%02u:%02u（读回校验通过）",
             (unsigned)local->year, (unsigned)local->month, (unsigned)local->day,
             (unsigned)local->hour, (unsigned)local->minute, (unsigned)local->second);
    return ESP_OK;
}

// ---------- 表盘时钟服务 ----------

static void clock_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        rtc_datetime_t dt;
        bool os = false;
        if (rtc_pcf_read(&dt, &os) == ESP_OK) {
            s_clock_last = dt;
            s_clock_valid = true;
            s_clock_fail_cnt = 0;
        } else {
            s_clock_fail_cnt++;
            s_clock_read_errs++;
            if (s_clock_read_errs % CLOCK_FAIL_LOG_EVERY == 1u) {
                ESP_LOGW(TAG, "clock read failed %" PRIu32 " times（OS=%d）",
                         s_clock_read_errs, (int)os);
            }
            if (s_clock_fail_cnt >= CLOCK_FAIL_INVALIDATE) {
                s_clock_valid = false; // 连续失败 → 失效显示"未对时"
            }
        }
    }
}

esp_err_t rtc_pcf_clock_service_start(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "rtc not init");
    BaseType_t ok = xTaskCreate(clock_task, "rtc_clock", CLOCK_TASK_STACK, NULL, 1, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "clock task create failed");
    ESP_LOGI(TAG, "clock service up (1Hz)");
    return ESP_OK;
}

void rtc_pcf_clock_snapshot(rtc_datetime_t *out, bool *valid)
{
    if (out != NULL) {
        *out = s_clock_last; // 瞬态副本
    }
    if (valid != NULL) {
        *valid = s_clock_valid;
    }
}
