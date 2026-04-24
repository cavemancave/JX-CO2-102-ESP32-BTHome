/*
 * gap.c — BLE GAP 广播与连接管理
 *
 * 负责 BLE 广播的初始化、启动、更新和连接事件处理。
 * 广播格式采用 BTHome v2（UUID 0xFCD2），Home Assistant 可自动发现 CO2 实体。
 *
 * 广播模式：Connectable Undirected（可连接），支持 GATT 配置的同时保持 BTHome 广播。
 * 连接期间暂停广播更新，断开后自动恢复广播。
 *
 * BTHome v2 Service Data 格式（6 字节）：
 *   [D2 FC]  UUID 0xFCD2，小端序
 *   [40]     info byte：version=2（bit7:5=010），unencrypted（bit0=0）
 *   [12]     Object ID：CO2，uint16，单位 ppm
 *   [LL HH]  CO2 浓度，uint16 小端序，范围 0-5000 ppm
 */
#include "gap.h"
#include "common.h"

/* ── 模块私有变量 ────────────────────────────────────────────────────────── */
static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static struct ble_gap_adv_params s_adv_params = {0};
static struct ble_hs_adv_fields s_adv_fields = {0};
static struct ble_hs_adv_fields s_rsp_fields = {0};
static uint8_t s_bthome_svc_data[6] = {0}; /* BTHome Service Data payload */
static bool s_connected = false;            /* 当前是否有 BLE 连接 */

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* 将 6 字节 BT 地址格式化为 "XX:XX:XX:XX:XX:XX" 字符串 */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/*
 * build_adv_fields_with_co2 — 构建 BTHome v2 广播 payload
 * ppm 超过 5000 时截断（传感器量程上限）。
 */
static void build_adv_fields_with_co2(uint16_t ppm) {
    if (ppm > 5000) ppm = 5000;
    s_bthome_svc_data[0] = 0xD2;                        /* UUID16 LSB */
    s_bthome_svc_data[1] = 0xFC;                        /* UUID16 MSB → 0xFCD2 */
    s_bthome_svc_data[2] = 0x40;                        /* BTHome v2, unencrypted */
    s_bthome_svc_data[3] = 0x12;                        /* Object ID: CO2 */
    s_bthome_svc_data[4] = (uint8_t)(ppm & 0xFF);       /* ppm LSB */
    s_bthome_svc_data[5] = (uint8_t)((ppm >> 8) & 0xFF);/* ppm MSB */

    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.svc_data_uuid16     = s_bthome_svc_data;
    s_adv_fields.svc_data_uuid16_len = sizeof(s_bthome_svc_data);
}

/*
 * start_advertising — 启动 BLE 广播
 * 广播包含 BTHome CO2 数据（初始值 0），扫描响应包含设备名称。
 * 使用 Connectable Undirected 模式，允许 nRF Connect 等工具连接配置。
 */
static void start_advertising(void) {
    int rc;

    /* 广播字段：BTHome Service Data，初始 CO2=0 */
    build_adv_fields_with_co2(0);
    rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) { ESP_LOGE(TAG, "set adv fields: %d", rc); return; }

    /* 扫描响应：设备完整名称（"JX-CO2-102 Sensor"） */
    memset(&s_rsp_fields, 0, sizeof(s_rsp_fields));
    const char *name = ble_svc_gap_device_name();
    s_rsp_fields.name            = (uint8_t *)name;
    s_rsp_fields.name_len        = strlen(name);
    s_rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&s_rsp_fields);
    if (rc != 0) { ESP_LOGE(TAG, "set rsp fields: %d", rc); return; }

    /* 广播参数：可连接，通用可发现 */
    memset(&s_adv_params, 0, sizeof(s_adv_params));
    s_adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    s_adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &s_adv_params, gap_event_handler, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "start adv: %d", rc); return; }
    ESP_LOGI(TAG, "advertising started");
}

/*
 * gap_event_handler — GAP 事件回调
 * 处理连接建立和断开事件，断开后自动重启广播。
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_connected = (event->connect.status == 0);
        if (s_connected)
            ESP_LOGI(TAG, "connected, handle=%d", event->connect.conn_handle);
        else
            start_advertising(); /* 连接失败时重启广播 */
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        start_advertising(); /* 断开后恢复广播 */
        break;
    default:
        break;
    }
    return 0;
}

/* ── 公开接口 ────────────────────────────────────────────────────────────── */

/*
 * adv_init — 初始化 BLE 地址并启动广播
 * 由 on_stack_sync 回调调用，确保 NimBLE 栈就绪后执行。
 */
void adv_init(void) {
    int rc;
    char addr_str[18] = {0};

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "no bt address"); return; }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer addr type: %d", rc); return; }

    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) { ESP_LOGE(TAG, "copy addr: %d", rc); return; }

    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);
    start_advertising();
}

/*
 * gap_update_co2 — 用新的 CO2 值更新广播包
 * 连接期间跳过更新（避免干扰 GATT 通信），断开后自动恢复。
 * 由 co2_uart_task 在达到 report_interval 时调用。
 */
void gap_update_co2(uint16_t ppm) {
    if (s_connected) return; /* 连接期间不更新广播 */
    int rc;
    build_adv_fields_with_co2(ppm);
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) { ESP_LOGE(TAG, "update adv fields: %d", rc); return; }
    ble_gap_adv_rsp_set_fields(&s_rsp_fields);
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &s_adv_params, gap_event_handler, NULL);
    if (rc != 0) ESP_LOGE(TAG, "restart adv: %d", rc);
}

/*
 * gap_init — 初始化 GAP service，设置设备名称和外观
 * 必须在 nimble_port_init() 之后调用。
 */
int gap_init(void) {
    int rc;
    ble_svc_gap_init();
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) { ESP_LOGE(TAG, "set name: %d", rc); return rc; }
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0) { ESP_LOGE(TAG, "set appearance: %d", rc); return rc; }
    return 0;
}
