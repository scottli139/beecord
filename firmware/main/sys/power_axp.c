// Beecord AXP2101 实机电源适配实现（power_axp.c，M5.4 二阶段）。
//
// 寄存器依据（X-power AXP2101 数据手册 §6.11 + 官方 XPowersLib 示例）：
//   0x00 STATUS1  bit3  = 电池在位（不在位则本次样本无效；bit0 是限流状态）
//   0x01 STATUS2  bit[6:5] = 充放电态（01 充电 / 10 放电 / 00 待机）
//   0x34/0x35     VBAT ADC（低 8 位 / 高 5 位，LSB 1mV，读 H5L8）
//   0xA4          电量百分比 0~100（Fuel Gauge）
//   设备地址 0x34（与 PMIC/RTC/codec 共享总线，仅加设备不加总线）
#include "power_axp.h"

#include <inttypes.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui/ui_task.h"

static const char *TAG = "power";

#define AXP_ADDR 0x34u
#define REG_STATUS1 0x00u
#define REG_STATUS2 0x01u
#define REG_VBAT_L 0x34u
#define REG_PERCENT 0xA4u

#define POLL_PERIOD_MS 1000u
#define POLL_TASK_PRIO 1 // 电源轮询：低于 UI(3)/按键(2)，1Hz 足够
#define POLL_TASK_STACK 4096u // 与 RTC 同为 I2C + ESP 日志调用链，预留栈保护余量
#define FAIL_LOG_EVERY 32u // 读取失败限频日志

static i2c_master_dev_handle_t s_dev;
static power_t s_power;
static float s_vbat_mv;
static uint8_t s_raw_pct;
static bool s_charging;
static uint32_t s_read_ok;
static uint32_t s_read_errs;

// 寄存器读：先写 reg 地址再读（新驱动单 API 完成 restart）
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, uint32_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

// 单次采样：任意一步失败即返回错误（降级跳过本次，不 abort）
static esp_err_t sample_once(void)
{
    uint8_t st1 = 0;
    uint8_t st2 = 0;
    uint8_t vbat[2] = { 0 };
    uint8_t pct = 0;

    ESP_RETURN_ON_ERROR(read_regs(REG_STATUS1, &st1, 1), TAG, "read STATUS1");
    ESP_RETURN_ON_ERROR(read_regs(REG_STATUS2, &st2, 1), TAG, "read STATUS2");
    ESP_RETURN_ON_ERROR(read_regs(REG_VBAT_L, vbat, 2), TAG, "read VBAT");
    ESP_RETURN_ON_ERROR(read_regs(REG_PERCENT, &pct, 1), TAG, "read PERCENT");

    if ((st1 & 0x08u) == 0u) {
        return ESP_ERR_NOT_FOUND; // STATUS1 bit3 电池不在位：按无效样本处理
    }
    s_vbat_mv = (float)(((uint16_t)(vbat[1] & 0x1Fu) << 8) | vbat[0]);
    s_raw_pct = pct > 100u ? 100u : pct;
    s_charging = ((st2 >> 5) & 0x03u) == 0x01u;
    return ESP_OK;
}

static void power_poll_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_PERIOD_MS));
        if (sample_once() != ESP_OK) {
            s_read_errs++;
            if (s_read_errs % FAIL_LOG_EVERY == 1u) {
                ESP_LOGW(TAG, "AXP read failed %" PRIu32 " times（共 %" PRIu32
                             " 次），跳过本次采样",
                         s_read_errs, s_read_ok + s_read_errs);
            }
            continue; // 降级：不喂样本（平滑与阈值不受坏点影响）
        }
        s_read_ok++;
        const uint8_t events = power_feed_sample(&s_power, (float)s_raw_pct, s_charging);
        // 通知 UI：code=显示百分比（0xFF=未知），value=充电位 | 事件位<<1
        ui_msg_t msg = {
            .src = UI_MSG_SRC_PWR,
            .code = (int32_t)power_display_pct(&s_power),
            .value = (int32_t)(s_charging ? 1u : 0u) | ((int32_t)events << 1),
        };
        ui_msg_post(&msg); // 队列满丢弃：1Hz 低采样，下一拍自然恢复
    }
}

esp_err_t power_axp_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "bsp i2c not ready（须在音频/触控初始化之后）");
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000, // 电源低频轮询，100k 足够且对共享总线友好
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG,
                        "axp device add failed");
    power_init(&s_power, NULL);

    BaseType_t ok = xTaskCreate(power_poll_task, "power", POLL_TASK_STACK, NULL,
                                POLL_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "power task create failed");
    ESP_LOGI(TAG, "AXP2101 power polling up (1Hz)");
    return ESP_OK;
}

void power_axp_get_debug(float *vbat_mv, uint8_t *raw_pct, uint8_t *smoothed_pct,
                         bool *charging, uint32_t *read_ok, uint32_t *read_errs)
{
    if (vbat_mv != NULL) {
        *vbat_mv = s_vbat_mv;
    }
    if (raw_pct != NULL) {
        *raw_pct = s_raw_pct;
    }
    if (smoothed_pct != NULL) {
        *smoothed_pct = power_display_pct(&s_power);
    }
    if (charging != NULL) {
        *charging = s_charging;
    }
    if (read_ok != NULL) {
        *read_ok = s_read_ok;
    }
    if (read_errs != NULL) {
        *read_errs = s_read_errs;
    }
}
