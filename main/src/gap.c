/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "gap.h"
#include "common.h"

/* Private function declarations */
inline static void format_addr(char *addr_str, uint8_t addr[]);
static void start_advertising(void);
static void build_adv_fields_with_co2(uint16_t ppm);

/* Private variables */
static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static struct ble_gap_adv_params s_adv_params = {0};
static struct ble_hs_adv_fields s_adv_fields = {0};
static struct ble_hs_adv_fields s_rsp_fields = {0};
/* BTHome v2 Service Data (UUID 0xFCD2): [D2 FC, info, id, value...] */
static uint8_t s_bthome_svc_data[6] = {0};

/* Private functions */
inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

static void start_advertising(void) {
    /* Local variables */
    int rc = 0;

    /* Build initial advertising fields with CO2=0 */
    build_adv_fields_with_co2(0);

    /* Set advertisement fields */
    rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return;
    }

    /* Prepare scan response with complete name only to save space */
    memset(&s_rsp_fields, 0, sizeof(s_rsp_fields));
    const char *name = ble_svc_gap_device_name();
    s_rsp_fields.name = (uint8_t *)name;
    s_rsp_fields.name_len = strlen(name);
    s_rsp_fields.name_is_complete = 1;

    /* Set scan response fields */
    rc = ble_gap_adv_rsp_set_fields(&s_rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
        return;
    }

    /* Set non-connectable and general discoverable mode to be a beacon */
    memset(&s_adv_params, 0, sizeof(s_adv_params));
    s_adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    s_adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Start advertising */
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &s_adv_params,
                           NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start advertising, error code: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising started!");
}

static void build_adv_fields_with_co2(uint16_t ppm) {
    /* Clamp ppm to 0..5000 */
    if (ppm > 5000) ppm = 5000;

    /* BTHome v2 (unencrypted), UUID 0xFCD2, CO2 id=0x12, value u16 LE */
    s_bthome_svc_data[0] = 0xD2; /* UUID16 L */
    s_bthome_svc_data[1] = 0xFC; /* UUID16 H */
    s_bthome_svc_data[2] = 0x40; /* info: v2 (2<<5), unencrypted */
    s_bthome_svc_data[3] = 0x12; /* object id: CO2 (uint16, factor 1) */
    s_bthome_svc_data[4] = (uint8_t)(ppm & 0xFF);       /* LSB */
    s_bthome_svc_data[5] = (uint8_t)((ppm >> 8) & 0xFF);/* MSB */

    memset(&s_adv_fields, 0, sizeof(s_adv_fields));

    /* Set advertising flags */
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Put CO2 data into Service Data (UUID16 = 0xFCD2, BTHome) */
    s_adv_fields.svc_data_uuid16 = s_bthome_svc_data;
    s_adv_fields.svc_data_uuid16_len = sizeof(s_bthome_svc_data);
}

/* Public functions */
void adv_init(void) {
    /* Local variables */
    int rc = 0;
    char addr_str[18] = {0};

    /* Make sure we have proper BT identity address set */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    /* Figure out BT address to use while advertising */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    /* Copy device address to addr_val */
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* Start advertising. */
    start_advertising();
}

void gap_update_co2(uint16_t ppm) {
    int rc;

    /* Rebuild advertising fields with new CO2 value */
    build_adv_fields_with_co2(ppm);

    /* If advertising is active, restart with updated data */
    if (ble_gap_adv_active()) {
        rc = ble_gap_adv_stop();
        if (rc != 0) {
            ESP_LOGW(TAG, "failed to stop advertising for update, rc=%d", rc);
        }
    }

    rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to update advertising data, rc=%d", rc);
        return;
    }

    /* Keep previous scan response (device name) */
    rc = ble_gap_adv_rsp_set_fields(&s_rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "failed to update scan response, rc=%d", rc);
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &s_adv_params,
                           NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to restart advertising, rc=%d", rc);
    }
}

int gap_init(void) {
    /* Local variables */
    int rc = 0;

    /* Initialize GAP service */
    ble_svc_gap_init();

    /* Set GAP device name */
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 DEVICE_NAME, rc);
        return rc;
    }

    /* Set GAP device appearance */
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device appearance, error code: %d", rc);
        return rc;
    }
    return rc;
}
