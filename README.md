# JX-CO2-102 Sensor (ESP-IDF + NimBLE + BTHome)

## Overview

Reads CO2 ppm from a UART sensor and broadcasts it over BLE using the BTHome v2 format so Home Assistant (with Bluetooth Proxy) can auto‑discover a CO2 sensor entity.

## Hardware

- Board: ESP32 family (tested with ESP32‑C3)
- UART sensor output: one line per second like `400 ppm\r\n`
- UART wiring: UART1 RX=GPIO4, TX=GPIO5, 9600 8N1 (sensor TX → ESP RX4, sensor RX → ESP TX5)

## Firmware Features

- Parses integer at line start; clamps to 0–5000 ppm; updates every second
- BTHome v2 Service Data (UUID 0xFCD2): `0x40` info (unencrypted, v2), `0x12` CO2, value `uint16` little‑endian (ppm)
- Device name in scan response: "JX-CO2-102 Sensor"

## Build & Flash

```sh
idf.py set-target esp32c3   # or your chip
idf.py build
idf.py -p <PORT> flash monitor
```

Exit monitor: Ctrl-].

## Home Assistant

- Requires HA Bluetooth integration or a Bluetooth Proxy
- Device is auto‑discovered as a BTHome device exposing CO2 (ppm)
- 若需要刷新名称：在 HA 设备页移除旧设备，等待重新发现

## Configuration

- UART pins/baud: edit `CO2_UART_RX_PIN`, `CO2_UART_TX_PIN`, `CO2_UART_BAUDRATE` in `main/main.c`
- Device name: edit `DEVICE_NAME` in `main/include/common.h`

## BLE Payload Details

- AD type: Service Data (16‑bit UUID)
- Bytes: `[D2 FC, 40, 12, PPM_L, PPM_H]`
  - `D2 FC` → UUID 0xFCD2
  - `40` → v2, unencrypted
  - `12` → CO2
  - `PPM_L/PPM_H` → ppm (little‑endian)

## Troubleshooting

- 识别成湿度：检查对象 ID 是否为 `0x12`
- 数值颠倒：确认 ppm 以小端序写入（LSB 在前）
- 无数据：确认串口连线与波特率、传感器输出格式为 `400 ppm\r\n`
