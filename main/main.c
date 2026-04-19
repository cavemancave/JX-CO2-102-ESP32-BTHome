/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "common.h"
#include "gap.h"
#include "driver/uart.h"

/* Library function declarations */
void ble_store_config_init(void);

/* Private function declarations */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);
static void co2_uart_task(void *param);
static void co2_uart_init(void);

/* Private functions */
/*
 *  Stack event callback functions
 *      - on_stack_reset is called when host resets BLE stack due to errors
 *      - on_stack_sync is called when host has synced with controller
 */
static void on_stack_reset(int reason) {
    /* On reset, print reset reason to console */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) {
    /* On stack sync, do advertising initialization */
    adv_init();

    /* Initialize UART and start CO2 reader task */
    co2_uart_init();
    xTaskCreate(co2_uart_task, "co2_uart", 3*1024, NULL, 5, NULL);
}

static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    /* Task entry log */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /* This function won't return until nimble_port_stop() is executed */
    nimble_port_run();

    /* Clean up at exit */
    vTaskDelete(NULL);
}

/* UART configuration: UART1 RX=GPIO4, TX=GPIO5, 9600 8N1 */
#define CO2_UART_NUM      UART_NUM_1
#define CO2_UART_RX_PIN   4
#define CO2_UART_TX_PIN   5
#define CO2_UART_BAUDRATE 9600

static void co2_uart_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = CO2_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CO2_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CO2_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(CO2_UART_NUM, CO2_UART_TX_PIN, CO2_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void co2_uart_task(void *param) {
    uint8_t rx_buf[64];
    char line[64];
    size_t idx = 0;

    ESP_LOGI(TAG, "CO2 UART task started (RX=GPIO%d, TX=GPIO%d, %d bps)",
             CO2_UART_RX_PIN, CO2_UART_TX_PIN, CO2_UART_BAUDRATE);

    while (1) {
        int len = uart_read_bytes(CO2_UART_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(200));
        if (len > 0) {
            for (int i = 0; i < len; ++i) {
                char c = (char)rx_buf[i];
                if (c == '\r') {
                    continue; /* skip CR */
                }
                if (c == '\n') {
                    /* terminate and parse */
                    if (idx < sizeof(line)) {
                        line[idx] = '\0';
                        int ppm = 0;
                        if (sscanf(line, "%d", &ppm) == 1) {
                            if (ppm < 0) ppm = 0;
                            if (ppm > 5000) ppm = 5000;
                            ESP_LOGI(TAG, "CO2: %d ppm", ppm);
                            gap_update_co2((uint16_t)ppm);
                        }
                    }
                    idx = 0; /* reset for next line */
                } else {
                    if (idx + 1 < sizeof(line)) {
                        line[idx++] = c;
                    } else {
                        /* overflow: reset */
                        idx = 0;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    /* Local variables */
    int rc = 0;
    esp_err_t ret = ESP_OK;

    /* NVS flash initialization */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /* NimBLE host stack initialization */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /* GAP service initialization */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }
#endif

    /* NimBLE host configuration initialization */
    nimble_host_config_init();

    /* Start NimBLE host task thread and return */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);
    return;
}
