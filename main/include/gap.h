/*
 * gap.h — BLE GAP 广播接口
 *
 * 提供 BLE 广播初始化和 CO2 数据更新接口。
 * 广播格式：BTHome v2（UUID 0xFCD2），Home Assistant 可自动发现 CO2 实体。
 */
#ifndef GAP_SVC_H
#define GAP_SVC_H

#include "services/gap/ble_svc_gap.h"

/* BTHome 广播所需的 GAP 常量 */
#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_URI_PREFIX_HTTPS       0x17
#define BLE_GAP_LE_ROLE_PERIPHERAL     0x00

/* 初始化 GAP service，设置设备名称和外观，必须在 nimble_port_init() 后调用 */
int  gap_init(void);

/* 初始化 BLE 地址并启动广播，由 on_stack_sync 回调调用 */
void adv_init(void);

/*
 * 用新的 CO2 浓度值更新 BLE 广播包
 * 连接期间自动跳过（不干扰 GATT 通信）
 * ppm: CO2 浓度，单位 ppm，超过 5000 时截断
 */
void gap_update_co2(uint16_t ppm);

#endif // GAP_SVC_H
