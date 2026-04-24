/*
 * gatt.c — BLE GATT Server
 *
 * 实现一个自定义 Primary Service，包含 4 个 Characteristic，
 * 允许通过 nRF Connect 或其他 BLE 客户端连接后读写设备配置。
 * 写入操作同步保存到 NVS，并通过 UART 将新配置发送给传感器。
 *
 * Service UUID:  12345678-1234-1234-1234-123456789012
 *
 * Characteristic 列表：
 *   UUID ...01  Calibrate      Write only  写 0x01 触发零点校准
 *   UUID ...02  ReportInterval Read/Write  uint16 LE，BLE 广播更新间隔（秒，1-3600）
 *   UUID ...03  AutoCalib      Read/Write  uint8，0=关闭 1=开启传感器 24h 自动校准
 *   UUID ...04  CommMode       Read/Write  uint8，1=主动上报 2=被动问询（MODBUS）
 *
 * nRF Connect 写入格式（小端序）：
 *   ReportInterval: 例如 300s → 写 "2C 01"
 *   AutoCalib:      开启 → 写 "01"，关闭 → 写 "00"
 *   CommMode:       主动 → 写 "01"，问询 → 写 "02"
 *   Calibrate:      触发 → 写 "01"
 */
#include "gatt.h"
#include "common.h"
#include "gap.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gatt/ble_svc_gatt.h"

/* UART 命令函数，实现在 main.c */
extern void uart_send_calibration(void);
extern void uart_send_comm_mode(uint8_t mode);
extern void uart_send_auto_calib(uint8_t enable);

/* 指向全局配置结构体，由 gatt_init() 注入 */
static co2_config_t *s_cfg;

/* ── UUID 定义 ──────────────────────────────────────────────────────────────
 * 128-bit UUID 在 BLE_UUID128_INIT 中以小端字节序排列。
 * 逻辑 UUID: 12345678-1234-1234-1234-1234567890XX
 *            其中 XX = 12(service), 01/02/03/04(characteristics)
 */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x12,0x90,0x78,0x56,0x34,0x12,0x34,0x12,
    0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x12);

static const ble_uuid128_t chr_calib_uuid = BLE_UUID128_INIT(
    0x12,0x90,0x78,0x56,0x34,0x12,0x34,0x12,
    0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x01);

static const ble_uuid128_t chr_interval_uuid = BLE_UUID128_INIT(
    0x12,0x90,0x78,0x56,0x34,0x12,0x34,0x12,
    0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x02);

static const ble_uuid128_t chr_auto_calib_uuid = BLE_UUID128_INIT(
    0x12,0x90,0x78,0x56,0x34,0x12,0x34,0x12,
    0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x03);

static const ble_uuid128_t chr_comm_mode_uuid = BLE_UUID128_INIT(
    0x12,0x90,0x78,0x56,0x34,0x12,0x34,0x12,
    0x34,0x12,0x34,0x12,0x78,0x56,0x34,0x04);

/* ── Characteristic 访问回调 ────────────────────────────────────────────── */

/* Calibrate: Write only，写 0x01 触发零点校准 */
static int chr_calib_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t val = 0;
    os_mbuf_copydata(ctxt->om, 0, 1, &val);
    if (val == 0x01) uart_send_calibration();
    return 0;
}

/* ReportInterval: Read/Write，uint16 LE，范围 1-3600 秒 */
static int chr_interval_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &s_cfg->report_interval, 2);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t val = 0;
        os_mbuf_copydata(ctxt->om, 0, 2, &val);
        if (val < 1) val = 1;
        if (val > 3600) val = 3600;
        s_cfg->report_interval = val;
        nvs_config_save(s_cfg);
        ESP_LOGI(TAG, "GATT: interval=%us", val);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* AutoCalib: Read/Write，uint8，0=关 1=开 */
static int chr_auto_calib_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &s_cfg->auto_calib, 1);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t val = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &val);
        val = val ? 1 : 0;
        s_cfg->auto_calib = val;
        nvs_config_save(s_cfg);
        uart_send_auto_calib(val); /* 立即发送给传感器 */
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* CommMode: Read/Write，uint8，1=主动上报 2=被动问询 */
static int chr_comm_mode_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, &s_cfg->comm_mode, 1);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t val = 0;
        os_mbuf_copydata(ctxt->om, 0, 1, &val);
        if (val != 1 && val != 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_cfg->comm_mode = val;
        nvs_config_save(s_cfg);
        uart_send_comm_mode(val); /* 立即切换传感器模式 */
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT Service 定义表 ────────────────────────────────────────────────── */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &chr_calib_uuid.u,      .access_cb = chr_calib_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_interval_uuid.u,   .access_cb = chr_interval_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_auto_calib_uuid.u, .access_cb = chr_auto_calib_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &chr_comm_mode_uuid.u,  .access_cb = chr_comm_mode_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { 0 } /* 终止符 */
        },
    },
    { 0 } /* 终止符 */
};

/*
 * gatt_init — 注册 GATT service，必须在 nimble_port_init() 之后、
 *             on_stack_sync 之前调用（即在 app_main 中调用）。
 */
int gatt_init(co2_config_t *cfg) {
    s_cfg = cfg;
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) return rc;
    return ble_gatts_add_svcs(s_svcs);
}
