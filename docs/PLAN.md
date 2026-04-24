# Implementation Plan: HA-Controllable CO2 Sensor

## Goal

Add bidirectional BLE control so Home Assistant can:
- Trigger manual calibration (button)
- Set BLE report interval (number)
- Toggle 24h auto-calibration (switch)
- Switch sensor comm mode: active/query (select)

All settings persist across power cycles via NVS.

---

## Architecture

```
[JX-CO2-102] --UART--> [ESP32-C3]
                            |
                   BLE connectable (BTHome v2)
                            |
                    [HA BTHome integration]
                            |
              button / number / switch / select
```

**Key decision:** Device changed from non-connectable beacon to connectable peripheral. BTHome v2 advertisement payload unchanged — HA still auto-discovers CO2 entity. GATT server added for control.

---

## GATT Service

Custom 128-bit Service UUID: `12345678-1234-1234-1234-123456789012`

| # | Characteristic | UUID suffix | Properties | Data | Behavior |
|---|---|---|---|---|---|
| 1 | Calibrate | `...01` | Write | `uint8` (write `0x01`) | Sends `FF 01 05 07 00 00 00 00 F4` to sensor |
| 2 | Report Interval | `...02` | Read/Write | `uint16` seconds (1–3600) | Throttles BLE adv update rate; saved to NVS |
| 3 | Auto Calibration | `...03` | Read/Write | `uint8` (0/1) | Sends open/close cmd to sensor; saved to NVS |
| 4 | Comm Mode | `...04` | Read/Write | `uint8` (1=active, 2=query) | Sends mode cmd to sensor; saved to NVS |

---

## NVS Persistence

Namespace: `co2_cfg`

| Key | Type | Default |
|---|---|---|
| `interval` | u16 | 60 |
| `auto_cal` | u8 | 0 |
| `comm_mode` | u8 | 1 |

On every boot: load NVS → resend comm_mode and auto_calib commands to sensor → sensor state matches NVS state. No dependency on sensor internal EEPROM.

---

## Sensor UART Commands

| Function | TX bytes |
|---|---|
| Manual calibration | `FF 01 05 07 00 00 00 00 F4` |
| Active mode | `FF 01 03 01 00 00 00 00 FC` |
| Query mode | `FF 01 03 02 00 00 00 00 FB` |
| Auto-calib ON | `FF 01 05 05 00 00 00 00 F6` |
| Auto-calib OFF | `FF 01 05 06 00 00 00 00 F5` |
| Query CO2 (MODBUS) | `01 03 00 05 00 01 94 0B` |

Checksum for `0x03`/`0x05` commands: `(~sum_bytes_1_to_7) + 1`

---

## Comm Mode Behavior

- **Mode 1 (active):** Sensor pushes ASCII lines every 1s. ESP32 parses and throttles BLE update by `report_interval`.
- **Mode 2 (query):** ESP32 sends MODBUS query, parses 7-byte binary response. Query rate = `report_interval`.

---

## File Changes

```
Modified:
  main/main.c          — NVS init, GATT init, UART command functions, startup config replay
  main/src/gap.c       — connectable mode, GAP event handler, report_interval throttle
  main/include/gap.h   — added gap_set_report_interval()

New:
  main/src/gatt.c      — GATT server, 4 characteristics
  main/src/nvs_config.c
  main/include/gatt.h
  main/include/nvs_config.h
```

---

## HA Integration

Since GATT uses custom UUIDs, use **ESPHome BLE Client** as bridge:

```yaml
ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX"
    id: co2_sensor

button:
  - platform: ble_client
    ble_client_id: co2_sensor
    name: "CO2 Calibrate"
    service_uuid: "12345678-1234-1234-1234-123456789012"
    characteristic_uuid: "00000001-1234-1234-1234-123456789012"
    value: [0x01]

number:
  - platform: ble_client
    ble_client_id: co2_sensor
    name: "CO2 Report Interval"
    service_uuid: "12345678-1234-1234-1234-123456789012"
    characteristic_uuid: "00000002-1234-1234-1234-123456789012"
    min_value: 1
    max_value: 3600

switch:
  - platform: ble_client
    ble_client_id: co2_sensor
    name: "CO2 Auto Calibration"
    service_uuid: "12345678-1234-1234-1234-123456789012"
    characteristic_uuid: "00000003-1234-1234-1234-123456789012"

select:
  - platform: ble_client
    ble_client_id: co2_sensor
    name: "CO2 Comm Mode"
    service_uuid: "12345678-1234-1234-1234-123456789012"
    characteristic_uuid: "00000004-1234-1234-1234-123456789012"
    options:
      - "Active"
      - "Query"
```
