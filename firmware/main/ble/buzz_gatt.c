// Beecord GATT 服务表（M4.1）实现。
//
// 服务结构（BUZZLINK.md §2.1）：
//   Service 0E242009-7EF4-4AE0-B3B0-E3A59E58D47C（primary）
//     ├─ Cmd Write  A2268D42-7024-430F-B059-9A156436973D（write without response）
//     └─ Event Notify C22080F5-0E96-4823-8909-1DD2CE427D2D（notify）
//
// NimBLE 的 BLE_UUID128_INIT 以小端字节序存储 UUID（即规范文本 UUID 的
// 16 字节完全倒序）；例如 Service 规范首字节 0x0E 在宏里排末位 0x0e、
// 规范末字节 0x7C 在宏里排首位 0x7c。
#include "buzz_gatt.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"

static const char *TAG = "buzz_gatt";

// Service 0E242009-7EF4-4AE0-B3B0-E3A59E58D47C（字节序已倒序，见文件头）。
const ble_uuid128_t buzz_gatt_svc_uuid =
    BLE_UUID128_INIT(0x7c, 0xd4, 0x58, 0x9e, 0xa5, 0xe3, 0xb0, 0xb3, //
                     0xe0, 0x4a, 0xf4, 0x7e, 0x09, 0x20, 0x24, 0x0e);

// Cmd Write A2268D42-7024-430F-B059-9A156436973D（字节序已倒序）。
static const ble_uuid128_t s_cmd_write_uuid =
    BLE_UUID128_INIT(0x3d, 0x97, 0x36, 0x64, 0x15, 0x9a, 0x59, 0xb0, //
                     0x0f, 0x43, 0x24, 0x70, 0x42, 0x8d, 0x26, 0xa2);

// Event Notify C22080F5-0E96-4823-8909-1DD2CE427D2D（字节序已倒序）。
static const ble_uuid128_t s_event_notify_uuid =
    BLE_UUID128_INIT(0x2d, 0x7d, 0x42, 0xce, 0xd2, 0x1d, 0x09, 0x89, //
                     0x23, 0x48, 0x96, 0x0e, 0xf5, 0x80, 0x20, 0xc2);

// 写日志 hex dump 上限：超出截断打印并标注 +N（M4.1 仅观察，不处理）。
#define CMD_DUMP_MAX 32u

static uint16_t s_event_notify_handle;

// Cmd Write 访问回调（NimBLE host 任务上下文）。
// M4.1：仅日志 hex dump，不做协议处理；M4.2 起在此处把字节流推进 server 输入队列。
static int cmd_write_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[CMD_DUMP_MAX];
    const uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    const uint16_t want = om_len > CMD_DUMP_MAX ? CMD_DUMP_MAX : om_len;
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, want, &copied);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 单行 hex：截断时尾部标注 ...(+N)，N = 未打印字节数。
    char hex[CMD_DUMP_MAX * 2 + 16];
    size_t pos = 0;
    for (uint16_t i = 0; i < copied; i++) {
        pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02x", buf[i]);
    }
    if (om_len > copied) {
        snprintf(hex + pos, sizeof(hex) - pos, "...(+%u)", (unsigned)(om_len - copied));
    }
    ESP_LOGI(TAG, "cmd write len=%u data=%s", om_len, hex);
    return 0;
}

// Event Notify 无读写属性，访问回调恒拒绝（CCCD 订阅由协议栈自行处理）。
static int event_notify_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &buzz_gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_cmd_write_uuid.u,
                .access_cb = cmd_write_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_event_notify_uuid.u,
                .access_cb = event_notify_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_event_notify_handle,
            },
            {
                0, // 特征结束
            },
        },
    },
    {
        0, // 服务结束
    },
};

esp_err_t buzz_gatt_register(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

uint16_t buzz_gatt_event_notify_handle(void)
{
    return s_event_notify_handle;
}
