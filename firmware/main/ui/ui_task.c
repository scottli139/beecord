// Beecord 手表 UI 任务与页面框架（ui_task.c，M5.1）。
// 启动：bsp_display_start_with_config（LVGL 任务由 esp_lvgl_port 托管，优先级 3——
// 低于音频采集 5 与 buzz 服务 4，见 M5_PLAN §5.2）→ 建页面 → REPL 命令注册。
#include "ui_task.h"

#include <inttypes.h>
#include <stdio.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "audio/buzz_recorder.h"
#include "screen_pwr.h"
#include "scr_home.h"
#include "scr_rec.h"
#include "scr_shell.h"
#include "sys/keyd.h"
#include "sys/keyd_gpio.h"
#include "sys/power.h"
#include "ui_shutdown.h"
#include "theme.h"
#include "touch_indev.h"
#include "ui_console.h"
#include "ui_rec.h"
#include "ui_snapshot.h"
#include "ui_toast.h"

static const char *TAG = "ui";

#if LV_USE_LOG
// LVGL 日志桥接到 esp_log（sdkconfig 开 LV_USE_LOG 时生效）
static void lv_log_print_cb(lv_log_level_t level, const char *buf)
{
    switch (level) {
    case LV_LOG_LEVEL_ERROR:
        ESP_LOGE("lvgl", "%s", buf);
        break;
    case LV_LOG_LEVEL_WARN:
        ESP_LOGW("lvgl", "%s", buf);
        break;
    default:
        ESP_LOGI("lvgl", "%s", buf);
        break;
    }
}
#endif

// LVGL 任务优先级：音频采集（REC_TASK_PRIO 5）> buzz 服务（BUZZ_CMDS_TASK_PRIO 4）> UI 3
#define UI_TASK_PRIO 3
#define UI_TASK_STACK 8192
#define UI_MSG_QUEUE_LEN 16

// 三主页 tile 坐标（row 0）；设置在第 0 列第 1 行（主页上滑进入）
#define TILE_COL_HOME 0
#define TILE_COL_LIST 1
#define TILE_COL_SYNC 2
#define TILE_ROW_MAIN 0
#define TILE_ROW_SETTINGS 1

static QueueHandle_t s_msg_q;
static lv_obj_t *s_tv;
static lv_obj_t *s_scr_main;
static lv_obj_t *s_dots[3];
static lv_obj_t *s_dot_box;
static bool s_ready;
static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_indev;

// ---------- 页面指示点（3×6px gap 6；z 在 tileview 之上，仅三主页可见） ----------

static void dots_update(uint8_t col, bool visible)
{
    if (visible) {
        lv_obj_set_hidden(s_dot_box, false);
    } else {
        lv_obj_set_hidden(s_dot_box, true);
    }
    if (!visible) {
        return;
    }
    for (uint8_t i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_dots[i],
                                  i == col ? UI_COLOR_TEXT_PRIMARY : UI_COLOR_CARD2, 0);
    }
}

static void dots_create(lv_obj_t *screen)
{
    s_dot_box = lv_obj_create(screen);
    lv_obj_set_size(s_dot_box, 3 * UI_PAGE_DOT + 2 * UI_PAGE_DOT_GAP, UI_PAGE_DOT);
    lv_obj_set_style_bg_opa(s_dot_box, 0, 0);
    lv_obj_set_style_border_width(s_dot_box, 0, 0);
    lv_obj_set_style_pad_all(s_dot_box, 0, 0);
    lv_obj_set_flex_flow(s_dot_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dot_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dot_box, UI_PAGE_DOT_GAP, 0);
    lv_obj_align(s_dot_box, LV_ALIGN_BOTTOM_MID, 0, -UI_SAFE_BOTTOM);
    lv_obj_set_scrollable(s_dot_box, false);
    for (uint8_t i = 0; i < 3; i++) {
        s_dots[i] = lv_obj_create(s_dot_box);
        lv_obj_set_size(s_dots[i], UI_PAGE_DOT, UI_PAGE_DOT);
        lv_obj_set_style_radius(s_dots[i], UI_RADIUS_FULL, 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_dots[i], 0, 0);
    }
    dots_update(TILE_COL_HOME, true);
}

static void on_tile_changed(lv_event_t *e)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(s_tv);
    if (tile == NULL) {
        return;
    }
    // tile 是其父容器中第 N 个加入的子对象：index = row * 3 + col
    uint32_t idx = lv_obj_get_index(tile);
    dots_update((uint8_t)(idx % 3), idx / 3 == TILE_ROW_MAIN);
}

// ---------- 导航 ----------

void ui_nav_to_tile(uint8_t col, uint8_t row)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    lv_tileview_set_tile_by_index(s_tv, col, row, LV_ANIM_OFF);
    dots_update(col, row == TILE_ROW_MAIN);
    bsp_display_unlock();
}

void ui_nav_to_home(void)
{
    ui_nav_to_tile(TILE_COL_HOME, TILE_ROW_MAIN);
}

bool ui_is_main_screen_active(void)
{
    if (!s_ready) {
        return false;
    }
    return lv_screen_active() == s_scr_main;
}

// ---------- 消息队列 ----------

esp_err_t ui_msg_post(const ui_msg_t *msg)
{
    if (s_msg_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_msg_q, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui msg queue full, drop src=%d code=%d", (int)msg->src, (int)msg->code);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

// 按键事件分发（LVGL 任务上下文，锁已持有；ux-device §1 映射）
static void handle_key_msg(const ui_msg_t *msg)
{
    keyd_key_id_t key = (keyd_key_id_t)msg->code;
    keyd_event_t ev = (keyd_event_t)msg->value;
    switch (key) {
    case KEYD_KEY_BOOT:
        // M5.3 录音热键接管（ux-device §1 原则 1：任何状态含熄屏，
        // 单击=开始/停止；双击=暂停/继续；长按 1.5s=标记。亮屏不再由 BOOT 负责，走 PWR）
        if (ev == KEYD_EVENT_CLICK) {
            (void)ui_rec_request_toggle();
            lv_display_trigger_activity(lv_display_get_default());
        } else if (ev == KEYD_EVENT_DOUBLE_CLICK) {
            (void)ui_rec_request_pause_toggle();
        } else if (ev == KEYD_EVENT_LONG_PRESS_1_5S) {
            (void)ui_rec_request_mark();
        }
        // 注：BOOT 的 LONG_PRESS_4_5S 事件刻意不处理——功能键无关机语义
        //（ux-device §1：BOOT 4.5s 无映射），关机警告仅 PWR 触发。
        break;
    case KEYD_KEY_PWR:
        if (ev == KEYD_EVENT_CLICK) {
            // 熄屏 ⇄ 亮屏
            screen_pwr_set_on(screen_pwr_is_off());
        } else if (ev == KEYD_EVENT_DOUBLE_CLICK) {
            ui_nav_to_home(); // 回表盘主页
        } else if (ev == KEYD_EVENT_LONG_PRESS_4_5S) {
            // M5.4：关机警告层（z80，全屏纯黑"即将关机…"＋剩余秒数）。
            // AXP2101 硬件 ~6s 断电不可取消；层在 6s 未断电（松手）时 ~2.5s 自消退
            // （keyd 无释放通知的文档化短超时方案，ui_shutdown.h 有注记）。
            ui_shutdown_warn_show();
        }
        break;
    default:
        break;
    }
}

// 在 LVGL 任务上下文执行（lv_timer），天然持有 LVGL 锁
// M5.4：低电确认（power 纯逻辑已做连续样本+迟滞防抖）。未充电 <10% 且
// 有活动会话（录音/暂停中）→ 显式停止请求（绝不误启动空闲态）+ 重要 toast。
static void handle_low_battery(void)
{
    if (buzz_recorder_state() == BUZZ_REC_STATE_IDLE) {
        ESP_LOGW(TAG, "低电确认但无活动会话，忽略");
        return;
    }
    ui_toast_show("电量过低，已自动保存", true); // 重要级：熄屏先唤醒
    esp_err_t ret = ui_rec_request_stop_low_battery();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "低电停止请求下发失败: %s（不静默）", esp_err_to_name(ret));
        ui_toast_show("停止失败", true);
    }
}

static void msg_pump(lv_timer_t *timer)
{
    (void)timer;
    ui_msg_t msg;
    while (xQueueReceive(s_msg_q, &msg, 0) == pdTRUE) {
        switch (msg.src) {
        case UI_MSG_SRC_DEBUG:
            // code=1：快照请求（渲染栈需求大，必须在 LVGL 任务执行）
            if (msg.code == 1) {
                ui_snapshot_execute_from_ui_task();
            } else {
                printf("ui msg: DEBUG code=%d value=%d\n", (int)msg.code, (int)msg.value);
            }
            break;
        case UI_MSG_SRC_KEY:
            handle_key_msg(&msg);
            break;
        case UI_MSG_SRC_REC: {
            // 录音会话事件：录音页显隐 + 主页横幅（msg.code=buzz_rec_evt_t）
            buzz_rec_evt_t evt = (buzz_rec_evt_t)msg.code;
            scr_rec_on_rec_event(evt);
            scr_home_show_rec_banner(evt == BUZZ_REC_EVT_STARTED ||
                                     evt == BUZZ_REC_EVT_PAUSED ||
                                     evt == BUZZ_REC_EVT_RESUMED ||
                                     evt == BUZZ_REC_EVT_MARKED);
            if (evt == BUZZ_REC_EVT_STOPPED) {
                ui_toast_show("已保存", false);
            }
            break;
        }
        case UI_MSG_SRC_TIMER:
            // M5.1 定时类 UI 更新（如 REC pulse 心跳）在此扩展
            break;
        case UI_MSG_SRC_PWR: {
            // M5.4 电源轮询：code=显示百分比（0xFF 未知），value=充电位|事件位<<1
            const uint8_t pct = (uint8_t)(msg.code & 0xFF);
            const bool charging = (msg.value & 0x1) != 0;
            const uint8_t events = (uint8_t)((msg.value >> 1) & 0xFF);
            scr_home_set_power(pct, charging);
            if ((events & POWER_EVT_LOW_ON) != 0u) {
                handle_low_battery();
            }
            break;
        }
        default:
            // M5.3 rec / M5.4 ble+pwr 接入前仅留痕
            ESP_LOGD(TAG, "msg src=%d code=%d value=%d pending", (int)msg.src,
                     (int)msg.code, (int)msg.value);
            break;
        }
    }
}

// ---------- 初始化 ----------

// SH8601（QSPI 面板）刷新区需按偶数坐标对齐（BSP 同款 rounder，QSPI 双字节对齐要求）
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

// 显示链路装配：bsp_display_new（SPI 面板初始化）+ lvgl_port_add_disp（SPI 刷新路径）。
// 不用 bsp_display_start：该封装内部对 SPI 面板走 lvgl_port_add_disp_rgb，会把 RGB 专用
// 回调注册写进 SH8601 面板句柄（esp_rgb_panel_t 字段偏移越界，堆破坏），属 BSP 缺陷。
static lv_display_t *display_assemble(void)
{
    const bsp_display_config_t bsp_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_err_t ret = bsp_display_new(&bsp_cfg, &panel, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed (%s)", esp_err_to_name(ret));
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        // 40 行（410×40×2B=33KB）：DMA  capable 内部 RAM 池有限，
        // M5.2 静态新增（beep PCM 等）后 82KB 实测分配失败——33KB 余量充足
        .buffer_size = BSP_LCD_H_RES * 40,
        // 单缓冲：每次 flush 完成后 LVGL 才渲染下一区域，SPI io 串行化，
        // 与出厂示例同款配置（双缓冲 + 每 flush 提前 ready 会把 LVGL 9.6 的
        // flushing 状态机打爆——实机 taskLVGL 卡死 wait_for_flushing 实锤）
        .double_buffer = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags =
            {
                .buff_dma = true,   // SPI DMA 只能可靠访问内部 RAM（PSRAM 经 cache 会丢完成中断，
                                    // 实机实锤 LVGL 卡死 wait_for_flushing）；BSP 同款取舍
                .buff_spiram = false,
                .sw_rotate = false,
                .swap_bytes = true, // BSP_LCD_BIGENDIAN：SH8601 QSPI 需字节交换
            },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    return disp;
}

esp_err_t ui_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG, "ui already init");

    // LVGL 任务优先级 3：低于音频采集（REC_TASK_PRIO 5）与 buzz 服务（BUZZ_CMDS_TASK_PRIO 4）
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = UI_TASK_PRIO;
    port_cfg.task_stack = UI_TASK_STACK;
    port_cfg.task_max_sleep_ms = 500;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");
#if LV_USE_LOG
    lv_log_register_print_cb(lv_log_print_cb);
#endif

    lv_display_t *disp = display_assemble();
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "display assemble failed");

    // 触摸：复刻 BSP 的 FT5x06 配置但 int_gpio=NC——esp_lvgl_port 对带 INT 的触摸
    // 会把 indev 设为 LV_INDEV_MODE_EVENT（靠中断唤醒读取，本板 INT 链路未验证，
    // M5.1 实测该模式下触摸全灭）；先用 LVGL 标准轮询模式，INT 模式 M5.2 再评估。
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &tp_io_cfg, &tp_io), TAG,
        "touch io failed");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &tp), TAG,
                        "ft5x06 init failed");
    s_tp = tp;
    // M5.2 缺陷修复：弃 esp_lvgl_port 触摸胶层（其读回调对读错误 ESP_ERROR_CHECK，
    // 共享 I2C 偶发 ESP_ERR_INVALID_STATE → abort 整机重启，实机两次实锤）——
    // 原生 LVGL indev 自建读回调（touch_indev.c），读失败保留上次坐标仅计数告警。
    lv_indev_t *indev = ui_touch_indev_create(disp, tp);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "touch indev create failed");
    s_indev = indev;

    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness init failed");

    bsp_display_lock(0);

    s_scr_main = lv_obj_create(NULL);
    ui_theme_apply_screen(s_scr_main);

    s_tv = lv_tileview_create(s_scr_main);
    lv_obj_set_size(s_tv, UI_SCREEN_W, UI_SCREEN_H);
    // 注意：tileview 的手势切换依赖 LVGL 滚动实现，禁止 scrollable 会让滑动全灭（M5.1 实测）；
    // 方向限制交给各 tile 的 LV_DIR_* 标志（ux-device §3 导航图）
    ui_theme_apply_page(s_tv);

    // 主页上滑进设置：LVGL scroll_dir 语义 = "允许内容滚向 X 看到 X 侧边缘"——
    // 手指上滑露出下方 tile 需 LV_DIR_BOTTOM（曾误写 LV_DIR_TOP，M5.1 实机实测纠正）；
    // 设置页下滑返回（露出上方主页）需 LV_DIR_TOP
    lv_obj_t *tile_home = lv_tileview_add_tile(s_tv, TILE_COL_HOME, TILE_ROW_MAIN,
                                               LV_DIR_HOR | LV_DIR_BOTTOM);
    lv_obj_t *tile_list = lv_tileview_add_tile(s_tv, TILE_COL_LIST, TILE_ROW_MAIN,
                                               LV_DIR_HOR);
    lv_obj_t *tile_sync = lv_tileview_add_tile(s_tv, TILE_COL_SYNC, TILE_ROW_MAIN,
                                               LV_DIR_HOR);
    lv_obj_t *tile_settings = lv_tileview_add_tile(s_tv, TILE_COL_HOME, TILE_ROW_SETTINGS,
                                                   LV_DIR_TOP);
    ui_theme_apply_page(tile_home);
    ui_theme_apply_page(tile_list);
    ui_theme_apply_page(tile_sync);
    ui_theme_apply_page(tile_settings);
    scr_home_create(tile_home);
    scr_list_create(tile_list);
    scr_sync_create(tile_sync);
    scr_settings_create(tile_settings);
    scr_rec_create();   // 模态录音页（M5.3）
    ui_toast_init();    // toast（挂 layer_top，跨页面）

    lv_obj_add_event_cb(s_tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    dots_create(s_scr_main);

    // 熄屏管理器（D6）：黑层覆盖 + indev 停用 + 亮度 0；BOOT 键轮询临时唤醒，
    // M5.2 由 keyd 事件替换（screen_pwr.c 有注记）
    esp_err_t pwr_ret = screen_pwr_init(s_scr_main, indev);
    if (pwr_ret != ESP_OK) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "screen pwr init failed (%s)", esp_err_to_name(pwr_ret));
        return pwr_ret;
    }

    lv_screen_load(s_scr_main);
    bsp_display_brightness_set(100);

    bsp_display_unlock();

    s_msg_q = xQueueCreate(UI_MSG_QUEUE_LEN, sizeof(ui_msg_t));
    ESP_RETURN_ON_FALSE(s_msg_q != NULL, ESP_ERR_NO_MEM, TAG, "msg queue create failed");

    // M5.2：按键子系统（10ms 轮询 → keyd 状态机 → 本队列 → msg_pump 分发）
    ESP_RETURN_ON_ERROR(keyd_gpio_init(), TAG, "keyd gpio init failed");
    // M5.3：录音控制任务 + 事件回调（阻塞式 recorder API 与提示音都在该任务）
    ESP_RETURN_ON_ERROR(ui_rec_init(), TAG, "ui_rec init failed");

    // 消息泵跑在 LVGL 任务上下文，避免再引一把锁
    lv_timer_create(msg_pump, 50, NULL);

    ESP_RETURN_ON_ERROR(ui_console_register(), TAG, "ui console register failed");

    s_ready = true;
    ESP_LOGI(TAG, "UI up (LVGL task prio %d, tileview 3+1 pages)", UI_TASK_PRIO);

    // M5.3：开机 journal 恢复 → toast（"已恢复未保存录音"）
    char recovered[80];
    if (ui_rec_take_recovered(recovered, sizeof(recovered))) {
        // 重要级：开机时屏幕未亮，须唤醒后弹（M5.3 实机定案，ux-device §7）
        ui_toast_show("已恢复未保存录音", true);
    }
    return ESP_OK;
}

// 触摸调试（`ui touch`）：与 esp_lvgl_port 同一条 esp_lcd_touch 读取路径，
// 用于区分硬件/驱动层（此处全 0）与 LVGL 集成层（此处有值而 UI 无反应）问题。
void ui_debug_touch_dump(void)
{
    if (s_tp == NULL) {
        printf("touch not init\n");
        return;
    }
    esp_err_t r1 = esp_lcd_touch_read_data(s_tp);
    uint32_t t_total = 0, t_err = 0;
    ui_touch_indev_stats(&t_total, &t_err);
    printf("touch indev: reads=%" PRIu32 " errs=%" PRIu32 "\n", t_total, t_err);
    esp_lcd_touch_point_data_t td[2] = {0};
    uint8_t cnt = 0;
    esp_err_t r2 = esp_lcd_touch_get_data(s_tp, td, &cnt, 2);
    printf("read=%s cnt=%u", esp_err_to_name(r1), cnt);
    for (int i = 0; i < cnt && i < 2; i++) {
        printf("  #%d x=%u y=%u id=%u str=%u", i, td[i].x, td[i].y, td[i].track_id,
               td[i].strength);
    }
    printf(" (get=%s)", esp_err_to_name(r2));

    // LVGL indev 侧：状态/坐标（state 恒 RELEASED 而 esp_lcd_touch 有触点 = 集成层断）
    if (bsp_display_lock(500)) {
        lv_indev_t *indev = lv_indev_get_next(NULL);
        if (indev != NULL) {
            lv_point_t p = {0, 0};
            lv_indev_get_point(indev, &p);
            printf(" | lvgl state=%d pt=(%ld,%ld)", (int)lv_indev_get_state(indev),
                   (long)p.x, (long)p.y);
        } else {
            printf(" | lvgl: no indev");
        }
        bsp_display_unlock();
    }
    printf("\n");
}

bool ui_is_ready(void)
{
    return s_ready;
}
