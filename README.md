# mitsubishi2zigbee

> [!CAUTION]
> **Early development — prototype only. Not production-ready.**
>
> This project is experimental and under active development. Firmware, wiring, and Zigbee behavior may change without notice. Bugs, incomplete features, and unexpected HVAC behavior are possible.
>
> **Do not rely on this device for critical climate control.** Use at your own risk. Test thoroughly before leaving it unattended.

Zigbee gateway firmware for Mitsubishi heat pumps and air conditioners with a **CN105** service port.

An **ESP32-C6** talks to the indoor unit over UART (CN105 protocol) and exposes it as a native Zigbee HVAC thermostat — no Wi-Fi or MQTT bridge on the microcontroller.

```
Home Assistant / Zigbee coordinator  ── Zigbee ──►  ESP32-C6  ── UART ──►  Mitsubishi unit
```

Inspired by [gysmo38/mitsubishi2MQTT](https://github.com/gysmo38/mitsubishi2MQTT), but uses Zigbee clusters (`hvacThermostat` + `hvacFanCtrl`) instead of MQTT.

## Features

| Category | Details |
| --- | --- |
| **Modes** | Off, Heat, Cool, Auto, Fan only, Dry |
| **Setpoint** | 16–31 °C in 0.5 °C steps |
| **Fan speed** | Auto, Quiet (Smart), Low, Medium, High |
| **Vane** | Auto, positions 1–5, Swing |
| **Telemetry** | Room temperature (unit sensor), compressor running state |
| **Status LED** (WS2812) | Fast blue — scanning · Slow blue — network found, not paired · Green pulse — paired |

## Hardware

### Supported board

- [Waveshare ESP32-C6-Zero](https://www.waveshare.com/esp32-c6-zero.htm) (or any ESP32-C6 dev board with similar pinout)
- Built with **ESP-IDF** via **PlatformIO**

### CN105 wiring

The CN105 port provides 12 V, 5 V, and 5 V logic UART lines.

| CN105 pin | Signal | ESP32-C6 | Notes |
| --- | --- | --- | --- |
| 1 | 12 V | — | Do **not** use to power the ESP |
| 2 | GND | GND | Common ground |
| 3 | 5 V | 5 V / VBUS | Powers the ESP32-C6 |
| 4 | TX (unit) | GPIO 5 (RX) | 5 V logic — use a level shifter (5 V → 3.3 V) |
| 5 | RX (unit) | GPIO 4 (TX) | 3.3 V from the ESP is sufficient |

> **Warning:** Never feed 12 V into the ESP. Power the board from the CN105 5 V pin only.

## Build and flash

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)

### Commands

```bash
cd firmware
pio run --target upload    # build and flash
pio device monitor         # serial logs (115200 baud)
```

## Zigbee pairing

1. Flash the firmware and power the board from the CN105 port.
2. Put your Zigbee coordinator into pairing mode.
3. Watch the status LED:
   - Fast blue — searching for a network
   - Slow blue — network found, waiting to join
   - Green pulse — joined successfully
4. The device advertises as **`MZ-Zigbee-C6`** (vendor: Mitsubishi).

## Zigbee2MQTT integration

> **To write** — step-by-step setup guide coming soon.

A video tutorial will cover the full Zigbee2MQTT setup (external converter, `configuration.yaml`, pairing, and Home Assistant entities). Stay tuned.

In the meantime, the external converter source is already available in [`zigbee2mqtt_converter/mitsubishi_esp32c6.js`](zigbee2mqtt_converter/mitsubishi_esp32c6.js).

## Project layout

```
mitsubishi2zigbee/
├── hardware/                  # KiCad PCB (CN105 carrier for ESP32-C6-Zero)
├── firmware/                  # ESP-IDF / PlatformIO project
│   └── src/
│       ├── main.cpp           # Zigbee endpoint + status LED
│       └── cn105.cpp          # Mitsubishi CN105 UART driver
└── zigbee2mqtt_converter/     # Optional Z2M external converter

```

## License

See repository license file.
