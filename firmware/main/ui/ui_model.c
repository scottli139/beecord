#include "ui_model.h"

#include "esp_attr.h"

#include "device_model.h"
#include "scr_format.h"
#include "scr_home.h"
#include "scr_shell.h"
#include "ui_toast.h"

static EXT_RAM_BSS_ATTR device_snapshot_t s_snapshot;
static uint32_t s_notice_seq;

static void handle_notice(const device_snapshot_t *snapshot)
{
    if (snapshot->notice_seq == s_notice_seq) {
        return;
    }
    s_notice_seq = snapshot->notice_seq;
    switch (snapshot->notice) {
    case DEVICE_NOTICE_SETTING_SAVE_FAILED:
        ui_toast_show("已生效，保存失败", true);
        break;
    case DEVICE_NOTICE_FORMAT_DONE:
        ui_toast_show("格式化完成", false);
        break;
    case DEVICE_NOTICE_FORMAT_SAVE_FAILED:
        ui_toast_show("格式化完成，同步状态保存失败", true);
        break;
    case DEVICE_NOTICE_FORMAT_FAILED:
        ui_toast_show("格式化失败，请重试", true);
        break;
    case DEVICE_NOTICE_BUSY:
        ui_toast_show("录音或下载中，无法格式化", true);
        break;
    default:
        break;
    }
}

static void model_timer(lv_timer_t *timer)
{
    (void)timer;
    device_model_snapshot(&s_snapshot);
    scr_home_render(&s_snapshot);
    scr_list_render(&s_snapshot);
    scr_sync_render(&s_snapshot);
    scr_settings_render(&s_snapshot);
    scr_format_render(&s_snapshot);
    handle_notice(&s_snapshot);
}

void ui_model_start(void)
{
    scr_format_create();
    model_timer(NULL);
    lv_timer_create(model_timer, 100, NULL);
}
