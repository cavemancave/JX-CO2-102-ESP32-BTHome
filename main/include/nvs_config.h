/*
 * nvs_config.h — NVS 持久化配置接口
 *
 * 定义设备运行参数结构体及读写接口。
 * 参数掉电保存在 ESP32 NVS Flash（namespace: "co2_cfg"）。
 */
#pragma once
#include <stdint.h>

/* 设备运行配置，由 NVS 持久化，由 GATT 写入时更新 */
typedef struct {
    uint16_t report_interval; /* BLE 广播更新间隔，单位秒，范围 1-3600，默认 300 */
    uint8_t  auto_calib;      /* 传感器 24h 自动校准：0=关闭，1=开启，默认 0 */
    uint8_t  comm_mode;       /* 传感器通讯模式：1=主动上报，2=被动问询，默认 1 */
} co2_config_t;

/* 从 NVS 加载配置，首次上电时使用默认值 */
void nvs_config_load(co2_config_t *cfg);

/* 将配置保存到 NVS，GATT 写入后调用 */
void nvs_config_save(const co2_config_t *cfg);
