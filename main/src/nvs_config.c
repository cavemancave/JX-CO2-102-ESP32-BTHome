/*
 * nvs_config.c — NVS 持久化配置
 *
 * 负责将设备运行参数保存到 ESP32 内部 NVS Flash，掉电不丢失。
 * 设备上电时调用 nvs_config_load() 恢复上次配置，并通过 UART 重放给传感器，
 * 确保传感器状态与 NVS 记录一致，不依赖传感器自身 EEPROM。
 *
 * NVS namespace: "co2_cfg"
 * Keys:
 *   "interval"  u16  BLE 广播更新间隔（秒），默认 300
 *   "auto_cal"  u8   传感器 24h 自动校准开关，默认 0（关）
 *   "comm_mode" u8   传感器通讯模式 1=主动上报 2=被动问询，默认 1
 */
#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define NVS_NS "co2_cfg"
#define TAG    "nvs_config"

/* 首次上电（NVS 为空）时使用的默认值 */
#define DEFAULT_INTERVAL  300   /* 5 分钟 */
#define DEFAULT_AUTO_CAL  0     /* 自动校准关闭 */
#define DEFAULT_COMM_MODE 1     /* 主动上报模式 */

/*
 * nvs_config_load — 从 NVS 读取配置到 cfg
 * 若 NVS 中无对应 key（首次上电），保留结构体中的默认值。
 */
void nvs_config_load(co2_config_t *cfg) {
    /* 先填入默认值，NVS 读取失败时直接使用 */
    cfg->report_interval = DEFAULT_INTERVAL;
    cfg->auto_calib      = DEFAULT_AUTO_CAL;
    cfg->comm_mode       = DEFAULT_COMM_MODE;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint16_t v16;
    uint8_t  v8;
    if (nvs_get_u16(h, "interval",  &v16) == ESP_OK) cfg->report_interval = v16;
    if (nvs_get_u8 (h, "auto_cal",  &v8)  == ESP_OK) cfg->auto_calib      = v8;
    if (nvs_get_u8 (h, "comm_mode", &v8)  == ESP_OK) cfg->comm_mode       = v8;
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: interval=%us auto_cal=%u comm_mode=%u",
             cfg->report_interval, cfg->auto_calib, cfg->comm_mode);
}

/*
 * nvs_config_save — 将 cfg 写入 NVS
 * 每次通过 BLE GATT 修改参数后调用，确保重启后配置不丢失。
 */
void nvs_config_save(const co2_config_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "interval",  cfg->report_interval);
    nvs_set_u8 (h, "auto_cal",  cfg->auto_calib);
    nvs_set_u8 (h, "comm_mode", cfg->comm_mode);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: interval=%us auto_cal=%u comm_mode=%u",
             cfg->report_interval, cfg->auto_calib, cfg->comm_mode);
}
