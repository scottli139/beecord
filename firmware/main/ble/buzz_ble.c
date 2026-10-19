// Beecord BLE 基础设施（M4.1）实现：NimBLE 端口初始化、广播、GAP 事件。
//
// 广播布局（BUZZLINK.md §2.2，31B 限制）：
//   AD  = flags(general discoverable) + complete UUID128 list（BuzzLink Service）
//   ScanRsp = Complete Local Name（BCD-XXXX）
// 连接建立后请求连接参数更新（interval 15~30ms、latency 0、supervision 2s），
// 优先 PHY 设为 2M（BUZZLINK.md §2.1）；均按建议值，对端可覆盖。
#include "buzz_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "buzz_gatt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "buzz_ble";

// 设备名 "BCD-XXXX"：前缀 + MAC 末 2 字节大写 hex + NUL。
#define DEVICE_NAME_FMT "BCD-%02X%02X"
static char s_device_name[sizeof("BCD-FFFF")];

// 建议连接参数（BUZZLINK.md §2.1），1.25ms / 10ms 单位：
// interval 15~30ms、latency 0、supervision timeout 2s。
#define CONN_ITVL_MIN_1_25MS 12u // 15 ms
#define CONN_ITVL_MAX_1_25MS 24u // 30 ms
#define CONN_LATENCY 0u
#define CONN_SUPERVISION_TIMEOUT_10MS 200u // 2 s

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t s_conn_handle = BUZZ_BLE_CONN_HANDLE_NONE;
static uint16_t s_att_mtu = BLE_ATT_MTU_DFLT; // 23：BLE 默认值
static bool s_notify_subscribed;

static int buzz_ble_gap_event(struct ble_gap_event *event, void *arg);

// 启动可连接非定向广播；ble_gap_adv_start 注册的 gap event 回调即下方
// buzz_ble_gap_event（sync 回调与连接事件上下文均可安全调用，同 bleprph 模式）。
static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    // AD：flags + complete UUID128 list（2+16=18B，加 flags 3B 共 21B ≤ 31B）。
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &buzz_gatt_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv data set failed rc=%d", rc);
        return;
    }

    // Scan response：Complete Local Name（"BCD-XXXX" 8 字符，2+8=10B ≤ 31B）。
    memset(&fields, 0, sizeof(fields));
    fields.name = (const uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan rsp data set failed rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           buzz_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started, name=%s", s_device_name);
}

// GAP 事件入口（ble_gap_adv_start 注册）。
static int buzz_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGI(TAG, "connection failed status=%d, re-advertise", event->connect.status);
            s_conn_handle = BUZZ_BLE_CONN_HANDLE_NONE;
            start_advertising();
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        s_att_mtu = BLE_ATT_MTU_DFLT;
        s_notify_subscribed = false;
        if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
            ESP_LOGI(TAG,
                     "connected handle=%" PRIu16 " peer=%02X:%02X:%02X:%02X:%02X:%02X",
                     s_conn_handle, (unsigned)desc.peer_id_addr.val[5],
                     (unsigned)desc.peer_id_addr.val[4], (unsigned)desc.peer_id_addr.val[3],
                     (unsigned)desc.peer_id_addr.val[2], (unsigned)desc.peer_id_addr.val[1],
                     (unsigned)desc.peer_id_addr.val[0]);
        } else {
            ESP_LOGI(TAG, "connected handle=%" PRIu16, s_conn_handle);
        }

        // 建议连接参数（BUZZLINK.md §2.1）；对端可拒绝/覆盖，不阻塞验收。
        const struct ble_gap_upd_params upd = {
            .itvl_min = CONN_ITVL_MIN_1_25MS,
            .itvl_max = CONN_ITVL_MAX_1_25MS,
            .latency = CONN_LATENCY,
            .supervision_timeout = CONN_SUPERVISION_TIMEOUT_10MS,
        };
        rc = ble_gap_update_params(s_conn_handle, &upd);
        if (rc != 0) {
            ESP_LOGI(TAG, "conn param update request rc=%d (peer may override)", rc);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected handle=%" PRIu16 " reason=%u", s_conn_handle,
                 (unsigned)event->disconnect.reason);
        s_conn_handle = BUZZ_BLE_CONN_HANDLE_NONE;
        s_att_mtu = BLE_ATT_MTU_DFLT;
        s_notify_subscribed = false;
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        s_att_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated handle=%" PRIu16 " mtu=%" PRIu16, event->mtu.conn_handle,
                 event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == buzz_gatt_event_notify_handle()) {
            s_notify_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG,
                     "SUBSCRIBE handle=%" PRIu16 " notify cur=%" PRIu16 " prev=%" PRIu16,
                     event->subscribe.attr_handle, event->subscribe.cur_notify,
                     event->subscribe.prev_notify);
        }
        break;

    default:
        break;
    }
    return 0;
}

static void on_reset(int reason)
{
    ESP_LOGI(TAG, "host reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
        return;
    }

    // 优先 2M PHY（BUZZLINK.md §2.1）；官方 API 拼写即 prefered。
    rc = ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);
    if (rc != 0) {
        ESP_LOGE(TAG, "set preferred default PHY rc=%d", rc);
    }

    start_advertising();
}

static void host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t buzz_ble_init(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "read BT MAC failed");
    snprintf(s_device_name, sizeof(s_device_name), DEVICE_NAME_FMT, (unsigned)mac[4],
             (unsigned)mac[5]);

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init failed");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    esp_err_t ret = buzz_gatt_register();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gatt register failed");
        nimble_port_deinit();
        return ret;
    }

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "set gap device name rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    // 不设持久化 store：D7 无配对绑定，CCCD 订阅态由协议栈在连接内 RAM 维护，
    // 且 ble_store_config_init 在 ESP-IDF NimBLE 中无公开头文件声明。
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE init done, device=%s", s_device_name);
    return ESP_OK;
}

const char *buzz_ble_device_name(void)
{
    return s_device_name;
}

bool buzz_ble_is_connected(void)
{
    return s_conn_handle != BUZZ_BLE_CONN_HANDLE_NONE;
}

uint16_t buzz_ble_conn_handle(void)
{
    return s_conn_handle;
}

uint16_t buzz_ble_mtu(void)
{
    return s_att_mtu;
}

bool buzz_ble_notify_subscribed(void)
{
    return s_notify_subscribed;
}
