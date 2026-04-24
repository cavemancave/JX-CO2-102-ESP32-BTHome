/*
 * main.c — 应用主入口
 *
 * 系统架构：
 *   [JX-CO2-102 传感器] --UART--> [ESP32-C3] --BLE BTHome v2--> [Home Assistant]
 *                                      |
 *                               BLE GATT Server
 *                                      |
 *                              [nRF Connect 手机配置]
 *
 * 任务结构：
 *   NimBLE Host task  — NimBLE 协议栈主循环
 *   co2_uart_task     — UART 读取传感器数据，按 report_interval 节流更新 BLE 广播
 *   button_task       — 监听 BOOT 按钮（GPIO9），按下触发手动零点校准
 *
 * 上电初始化流程：
 *   1. nvs_flash_init()       初始化 NVS
 *   2. nvs_config_load()      从 NVS 读取配置（首次上电使用默认值）
 *   3. nimble_port_init()     初始化 NimBLE 协议栈
 *   4. gap_init()             注册 GAP service，设置设备名称
 *   5. gatt_init()            注册自定义 GATT service（4 个 characteristics）
 *   6. nimble_host_task 启动  → on_stack_sync 回调
 *   7. on_stack_sync:
 *      - adv_init()           启动 BLE 广播
 *      - co2_uart_init()      初始化 UART1
 *      - uart_apply_config()  重放 NVS 配置到传感器（掉电恢复）
 *      - 启动 co2_uart_task 和 button_task
 *
 * UART 命令（JX-CO2-102 协议）：
 *   uart_send_calibration()  FF 01 05 07 00 00 00 00 F4  零点校准
 *   uart_send_comm_mode(1)   FF 01 03 01 00 00 00 00 FC  切换主动上报模式
 *   uart_send_comm_mode(2)   FF 01 03 02 00 00 00 00 FB  切换被动问询模式
 *   uart_send_auto_calib(1)  FF 01 05 05 00 00 00 00 F6  开启 24h 自动校准
 *   uart_send_auto_calib(0)  FF 01 05 06 00 00 00 00 F5  关闭 24h 自动校准
 *
 * 校验和计算：cmd[8] = (~sum(cmd[1..7])) + 1
 */
#include "common.h"
#include "gap.h"
#include "gatt.h"
#include "nvs_config.h"
#include "driver/uart.h"
#include "driver/gpio.h"

/* NimBLE 存储配置初始化（链接自 NimBLE 库） */
void ble_store_config_init(void);

/* 函数前向声明 */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);
static void co2_uart_task(void *param);
static void co2_uart_init(void);
static void button_task(void *param);

/* ── 硬件引脚与 UART 配置 ────────────────────────────────────────────────── */
#define CALIB_BTN_GPIO    9     /* BOOT 按钮，active-low，内部上拉 */
#define CO2_UART_NUM      UART_NUM_1
#define CO2_UART_RX_PIN   4     /* 传感器 TX → ESP32 RX */
#define CO2_UART_TX_PIN   5     /* 传感器 RX ← ESP32 TX */
#define CO2_UART_BAUDRATE 9600

/* 全局配置，从 NVS 加载，由 GATT 写入时更新 */
static co2_config_t s_cfg;

/* ── UART 命令函数（由 gatt.c 通过 extern 调用）────────────────────────── */

/* 触发传感器零点校准（需在室外或 CO2 ≈ 400ppm 环境下使用） */
void uart_send_calibration(void) {
    const uint8_t cmd[] = {0xFF, 0x01, 0x05, 0x07, 0x00, 0x00, 0x00, 0x00, 0xF4};
    ESP_LOGI(TAG, "calibration triggered");
    uart_write_bytes(CO2_UART_NUM, cmd, sizeof(cmd));
}

/* 切换传感器通讯模式：mode=1 主动上报，mode=2 被动问询 */
void uart_send_comm_mode(uint8_t mode) {
    uint8_t cmd[] = {0xFF, 0x01, 0x03, mode, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) sum += cmd[i];
    cmd[8] = (~sum) + 1; /* 校验和 */
    ESP_LOGI(TAG, "comm_mode → %u", mode);
    uart_write_bytes(CO2_UART_NUM, cmd, sizeof(cmd));
}

/* 开启/关闭传感器内部 24h 自动校准：enable=1 开启，enable=0 关闭 */
void uart_send_auto_calib(uint8_t enable) {
    uint8_t sub = enable ? 0x05 : 0x06; /* 0x05=开启，0x06=关闭 */
    uint8_t cmd[] = {0xFF, 0x01, 0x05, sub, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) sum += cmd[i];
    cmd[8] = (~sum) + 1;
    ESP_LOGI(TAG, "auto_calib → %u", enable);
    uart_write_bytes(CO2_UART_NUM, cmd, sizeof(cmd));
}

/*
 * uart_apply_config — 上电时将 NVS 配置重放给传感器
 * 传感器掉电后内部状态未知，通过重发命令确保与 NVS 记录一致。
 */
static void uart_apply_config(void) {
    uart_send_comm_mode(s_cfg.comm_mode);
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_send_auto_calib(s_cfg.auto_calib);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── UART 初始化 ─────────────────────────────────────────────────────────── */
static void co2_uart_init(void) {
    const uart_config_t cfg = {
        .baud_rate  = CO2_UART_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CO2_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CO2_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CO2_UART_NUM, CO2_UART_TX_PIN, CO2_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* MODBUS RTU 查询命令：读寄存器 0x0005（CO2 浓度） */
static const uint8_t s_query_cmd[] = {0x01, 0x03, 0x00, 0x05, 0x00, 0x01, 0x94, 0x0B};

/*
 * co2_uart_task — CO2 数据读取任务
 *
 * 主动模式（comm_mode=1）：
 *   传感器每秒输出一行 ASCII，格式 "  1234 ppm\r\n"。
 *   逐字节拼行，解析首个整数，按 report_interval 节流更新 BLE 广播。
 *
 * 被动问询模式（comm_mode=2）：
 *   每隔 report_interval 秒发送 MODBUS RTU 查询，解析 7 字节二进制响应。
 *   响应格式：[01 03 02 HH LL CRC_L CRC_H]，CO2 = (HH<<8)|LL ppm。
 */
static void co2_uart_task(void *param) {
    uint8_t rx_buf[64];
    char line[64];
    size_t idx = 0;
    int64_t last_update = 0;

    ESP_LOGI(TAG, "CO2 UART task started (mode=%u, interval=%us)",
             s_cfg.comm_mode, s_cfg.report_interval);

    while (1) {
        int64_t now = esp_timer_get_time(); /* 微秒时间戳 */

        if (s_cfg.comm_mode == 2) {
            /* 被动问询模式 */
            if (now - last_update >= (int64_t)s_cfg.report_interval * 1000000LL) {
                uart_write_bytes(CO2_UART_NUM, s_query_cmd, sizeof(s_query_cmd));
                vTaskDelay(pdMS_TO_TICKS(200));
                int len = uart_read_bytes(CO2_UART_NUM, rx_buf, 7, pdMS_TO_TICKS(300));
                /* 验证响应头：地址=0x01，功能码=0x03，字节数=0x02 */
                if (len == 7 && rx_buf[0] == 0x01 && rx_buf[1] == 0x03 && rx_buf[2] == 0x02) {
                    uint16_t ppm = ((uint16_t)rx_buf[3] << 8) | rx_buf[4];
                    if (ppm > 5000) ppm = 5000;
                    ESP_LOGI(TAG, "CO2 (query): %d ppm", ppm);
                    gap_update_co2(ppm);
                    last_update = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            /* 主动上报模式：逐字节解析 ASCII 行 */
            int len = uart_read_bytes(CO2_UART_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(200));
            for (int i = 0; i < len; ++i) {
                char c = (char)rx_buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    line[idx] = '\0';
                    int ppm = 0;
                    if (sscanf(line, "%d", &ppm) == 1) {
                        if (ppm < 0) ppm = 0;
                        if (ppm > 5000) ppm = 5000;
                        /* 节流：仅在 report_interval 到期时更新广播 */
                        if (now - last_update >= (int64_t)s_cfg.report_interval * 1000000LL) {
                            last_update = now;
                            ESP_LOGI(TAG, "CO2: %d ppm", ppm);
                            gap_update_co2((uint16_t)ppm);
                        }
                    }
                    idx = 0;
                } else {
                    if (idx + 1 < sizeof(line)) line[idx++] = c;
                    else idx = 0; /* 行过长，丢弃重新开始 */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/*
 * button_task — BOOT 按钮监听任务
 * GPIO9 active-low，检测下降沿后 50ms 去抖，确认按下则触发校准。
 */
static void button_task(void *param) {
    bool last = true;
    while (1) {
        bool cur = gpio_get_level(CALIB_BTN_GPIO);
        if (!cur && last) {
            vTaskDelay(pdMS_TO_TICKS(50)); /* 去抖延时 */
            if (!gpio_get_level(CALIB_BTN_GPIO))
                uart_send_calibration();
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── NimBLE 栈回调 ───────────────────────────────────────────────────────── */

/* 栈异常复位时打印原因 */
static void on_stack_reset(int reason) {
    ESP_LOGI(TAG, "nimble stack reset, reason: %d", reason);
}

/*
 * on_stack_sync — NimBLE 栈就绪回调
 * 栈初始化完成后由 NimBLE 调用，在此启动广播和外设任务。
 */
static void on_stack_sync(void) {
    adv_init(); /* 启动 BLE 广播 */

    /* 初始化校准按钮 */
    gpio_set_direction(CALIB_BTN_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CALIB_BTN_GPIO, GPIO_PULLUP_ONLY);
    xTaskCreate(button_task, "button_task", 2*1024, NULL, 4, NULL);

    /* 初始化 UART，重放配置，启动读取任务 */
    co2_uart_init();
    uart_apply_config();
    xTaskCreate(co2_uart_task, "co2_uart", 3*1024, NULL, 5, NULL);
}

static void nimble_host_config_init(void) {
    ble_hs_cfg.reset_cb        = on_stack_reset;
    ble_hs_cfg.sync_cb         = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "nimble host task started");
    nimble_port_run(); /* 阻塞直到 nimble_port_stop() */
    vTaskDelete(NULL);
}

/* ── 应用入口 ────────────────────────────────────────────────────────────── */
void app_main(void) {
    /* 初始化 NVS Flash（BLE 配对信息和应用配置均存于此） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 从 NVS 加载配置（首次上电使用默认值：interval=300s, auto_calib=0, comm_mode=1） */
    nvs_config_load(&s_cfg);

    /* 初始化 NimBLE 协议栈 */
    ESP_ERROR_CHECK(nimble_port_init());

    /* 注册 GAP service（设备名称、外观） */
    int rc = gap_init();
    if (rc != 0) { ESP_LOGE(TAG, "gap_init: %d", rc); return; }

    /* 注册自定义 GATT service（校准/间隔/自动校准/通讯模式） */
    rc = gatt_init(&s_cfg);
    if (rc != 0) { ESP_LOGE(TAG, "gatt_init: %d", rc); return; }

    /* 配置并启动 NimBLE host 任务 */
    nimble_host_config_init();
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);
}
