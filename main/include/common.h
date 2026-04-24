/*
 * common.h — 公共头文件
 *
 * 所有模块共用的标准库、ESP-IDF、FreeRTOS 和 NimBLE 头文件，
 * 以及全局常量定义。
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/* 日志 TAG，所有模块统一使用 */
#define TAG         "CO2-Sensor"

/* BLE 广播设备名称（出现在扫描结果和 HA 设备列表中） */
#define DEVICE_NAME "JX-CO2-102 Sensor"

#endif // COMMON_H
