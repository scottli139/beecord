// 熄屏管理器实现（screen_pwr.c，M5.1）。
//
// D6 技术验证（M5_PLAN 要求实机实测，验证项随实机验证清单执行）：
//   bsp_display_backlight_off() 对本板 AMOLED（SH8601）的行为预期为"面板亮度命令
//   置 0"（BSP 内 bsp_display_brightness_set(0)，板级无独立背光脚）——视觉全灭、
//   触摸控制器不受影响、再 on（亮度 100）瞬时恢复，不重建帧缓冲。
//   注意它不是 panel sleep（esp_lcd_panel_disp_on_off(false)），像素驱动仍工作，
//   理论上仍有小电流；M5.1 叠加"纯黑覆盖层"双保险（即使亮度语义有差异也是纯黑画面），
//   熄屏期间无 UI 失效区 → LVGL 无 SPI 刷屏。功耗差未实测（无仪器），留 M5.6 审计。
#include "screen_pwr.h"

#include <stdio.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "theme.h"
#include "ui_task.h"

static const char *TAG = "screen_pwr";

// BOOT 键轮询周期/消抖：10ms 采样，连续 2 次低电平判按下（20ms 消抖）
#define KEY_POLL_MS 10
#define KEY_DEBOUNCE_CNT 2

static lv_obj_t *s_black;    // 熄屏黑层（z60：最后创建，位于指示点之上）
static lv_indev_t *s_indev;
static bool s_off;

// 熄屏三件套：纯黑覆盖 + 触摸停用 + 亮度 0。
// 内部自获取 LVGL 锁：lv_indev_enable/对象操作是 LVGL API；且亮度命令走 SPI 面板 io，
// 必须与 LVGL flush 互斥——否则 tx_param(同步) 与 tx_color(DMA) 并发会丢完成回调，
// LVGL 卡死在 wait_for_flushing（taskLVGL 看门狗实锤，M5.1 实机定位）。
// 锁为递归互斥，LVGL 任务上下文（定时器）调用安全。
bool screen_pwr_set_on(bool on)
{
    if (s_black == NULL) {
        return false;
    }
    bsp_display_lock(0);
    if (on == !s_off) {
        // 已是目标态：on 路径也要重置无操作计时——否则周期性"保持常亮"调用
        // （REPL 调试/未来下载防熄屏）无法推迟自动熄屏（M5.1 实锤的逻辑缺口）
        if (on) {
            lv_display_trigger_activity(lv_display_get_default());
        }
        bsp_display_unlock();
        return s_off;
    }
    if (on) {
        lv_obj_set_hidden(s_black, true);
        lv_indev_enable(s_indev, true);
        lv_display_trigger_activity(lv_display_get_default());
        s_off = false;
        ESP_LOGI(TAG, "screen on");
    } else {
        lv_obj_set_hidden(s_black, false);
        lv_indev_enable(s_indev, false);
        s_off = true;
        ESP_LOGI(TAG, "screen off");
    }
    // 亮度命令（SH8601 亮度 = SPI 同步轮询 tx_param）与 LVGL flush（异步 DMA
    // tx_color）交替会打挂 SPI 完成中断——实机实锤：之后 LVGL 永久卡死
    // wait_for_flushing。M5.1 熄屏先只用纯黑覆盖层（AMOLED 黑像素不发光，
    // 功耗与亮度 0 等效）；亮度命令留待 M5.2 按"渲染帧内发送"或官方修复后再接。
    // bsp_display_backlight_on/off 调用点保留在此注释旁以便恢复。
    bsp_display_unlock();
    return s_off;
}

bool screen_pwr_is_off(void)
{
    return s_off;
}

// 无操作监视：LVGL indev inactive time 达阈值即熄屏
static void off_monitor(lv_timer_t *timer)
{
    (void)timer;
    if (s_off || s_black == NULL) {
        return;
    }
    uint32_t idle = lv_display_get_inactive_time(lv_display_get_default());
    if (idle >= UI_SCREEN_OFF_MS) {
        screen_pwr_set_on(false);
    }
}

// BOOT 键（GPIO0，低=按下）轮询唤醒。
// 注：M5.1 临时方案——单击唤醒 + 重置无操作计时；M5.2 由 keyd 按键子系统
// 事件替换（届时录音热键也挂在 keyd 上，本轮询任务撤除）。
static void boot_key_poll(lv_timer_t *timer)
{
    (void)timer;
    static uint8_t low_cnt;
    static bool prev_pressed;
    bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
    low_cnt = pressed ? low_cnt + 1 : 0;
    bool stable = low_cnt >= KEY_DEBOUNCE_CNT;
    if (stable && !prev_pressed) {
        // 单击沿：熄屏则唤醒；亮屏则重置无操作计时（相当于"有操作"）
        if (s_off) {
            screen_pwr_set_on(true);
        } else {
            lv_display_trigger_activity(lv_display_get_default());
        }
    }
    prev_pressed = stable;
}

esp_err_t screen_pwr_init(lv_obj_t *screen, lv_indev_t *indev)
{
    ESP_RETURN_ON_FALSE(screen != NULL && indev != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "screen/indev null");

    // 熄屏层：全屏纯黑无内容、触控关闭由 indev 停用保证（DESIGN_SYSTEM §3 z60）
    s_black = lv_obj_create(screen);
    lv_obj_set_size(s_black, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_black, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_black, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_black, 0, 0);
    lv_obj_set_style_pad_all(s_black, 0, 0);
    lv_obj_set_style_radius(s_black, 0, 0);
    lv_obj_set_scrollable(s_black, false);
    lv_obj_set_clickable(s_black, false);
    lv_obj_set_hidden(s_black, true); // 最后创建天然位于 z 序顶层，等价 z60
    s_indev = indev;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // 悬空为高，按下接地
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio0 config failed");

    // 两个 timer 都跑在 LVGL 任务上下文，无需再加锁
    lv_timer_create(off_monitor, 500, NULL);
    lv_timer_create(boot_key_poll, KEY_POLL_MS, NULL);

    ESP_LOGI(TAG, "screen pwr manager up (off timeout %d ms, BOOT key poll %d ms)",
             UI_SCREEN_OFF_MS, KEY_POLL_MS);
    return ESP_OK;
}
