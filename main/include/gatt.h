/*
 * gatt.h — BLE GATT Server 接口
 *
 * 暴露 gatt_init() 供 app_main 调用。
 * GATT service 包含 4 个 characteristics，允许通过 nRF Connect 配置设备参数。
 */
#pragma once
#include "nvs_config.h"

/*
 * gatt_init — 注册自定义 GATT Primary Service
 * cfg: 指向全局配置结构体，characteristics 读写操作直接修改此结构体并持久化到 NVS。
 * 返回 0 表示成功，非 0 为 NimBLE 错误码。
 * 必须在 nimble_port_init() 之后、NimBLE host task 启动之前调用。
 */
int gatt_init(co2_config_t *cfg);
