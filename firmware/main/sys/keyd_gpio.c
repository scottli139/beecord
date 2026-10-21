// 按键 GPIO 实机绑定实现（keyd_gpio.c，M5.2）。
#include "keyd_gpio.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "keyd.h"
#include "ui/ui_task.h"

static const char *TAG = "keyd";

#define KEYD_POLL_MS 10
#define KEYD_TASK_STACK 2560
// 轮询任务优先级：低于 UI(3)/buzz(4)/音频(5)，高于空闲即可
#define KEYD_TASK_PRIO 2

// PWR 检测引脚（实机已坐实 GPIO10 高=按下；英文 Wiki 的 EXIO6 扩展器说法
// 经 i2cscan 全扫无未知地址排除，见 HARDWARE.md）。
#define PWR_GPIO GPIO_NUM_10
#define BOOT_GPIO GPIO_NUM_0

static keyd_t s_keys[KEYD_KEY_COUNT];

// 事件名表（日志/REPL 观测用）
static const char *s_evt_names[] = {
    [KEYD_EVENT_NONE] = "none",
    [KEYD_EVENT_CLICK] = "click",
    [KEYD_EVENT_DOUBLE_CLICK] = "double_click",
    [KEYD_EVENT_LONG_PRESS_1_5S] = "long_press_1.5s",
    [KEYD_EVENT_LONG_PRESS_4_5S] = "long_press_4.5s",
};

static void keyd_poll_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(KEYD_POLL_MS));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        bool levels[KEYD_KEY_COUNT] = {
            [KEYD_KEY_BOOT] = gpio_get_level(BOOT_GPIO) == 0, // 低=按下
            [KEYD_KEY_PWR] = gpio_get_level(PWR_GPIO) == 1,   // 高=按下（实机验证）
        };
        for (int k = 0; k < KEYD_KEY_COUNT; k++) {
            keyd_event_t ev = keyd_feed(&s_keys[k], levels[k], now_ms);
            if (ev == KEYD_EVENT_NONE) {
                continue;
            }
            // REPL 可直接观测按键事件（串口日志即控制台）
            ESP_LOGI(TAG, "key=%s event=%s", k == KEYD_KEY_BOOT ? "BOOT" : "PWR",
                     s_evt_names[ev]);
            ui_msg_t msg = {
                .src = UI_MSG_SRC_KEY,
                .code = k,
                .value = (int32_t)ev,
            };
            ui_msg_post(&msg); // 队列满丢弃由 UI 侧告警，按键不阻塞
        }
    }
}

esp_err_t keyd_gpio_init(void)
{
    for (int k = 0; k < KEYD_KEY_COUNT; k++) {
        keyd_init(&s_keys[k], NULL);
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_GPIO) | (1ULL << PWR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // BOOT 悬空高；PWR 外接上拉未知，先启用观测
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "key gpio config failed");

    // 观测点：上电即打印两路电平，REPL `keylvl` 可随时复查
    ESP_LOGI(TAG, "BOOT(GPIO%d)=%d PWR(GPIO%d)=%d", BOOT_GPIO,
             (int)gpio_get_level(BOOT_GPIO), PWR_GPIO, (int)gpio_get_level(PWR_GPIO));

    BaseType_t ok = xTaskCreate(keyd_poll_task, "keyd_poll", KEYD_TASK_STACK, NULL,
                                KEYD_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "keyd poll task create failed");
    return ESP_OK;
}

void keyd_gpio_dump_levels(void)
{
    printf("BOOT(GPIO%d)=%d PWR(GPIO%d)=%d\n", BOOT_GPIO,
           (int)gpio_get_level(BOOT_GPIO), PWR_GPIO, (int)gpio_get_level(PWR_GPIO));
}
