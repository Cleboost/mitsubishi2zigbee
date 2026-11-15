# Hardware

KiCad PCB for a [Waveshare ESP32-C6-Zero](https://www.waveshare.com/esp32-c6-zero.htm) carrier that plugs into a Mitsubishi **CN105** service port.

| Item | Details |
| --- | --- |
| MCU footprint | `Waveshare-ESP32:Waveshare-ESP32-C6-Zero_THT` (2×9 pin headers) |
| CN105 connector | `JST:JST_PA_B05B-PASK-1_1x05_P2.00mm_Vertical_CN105` |
| Level shifting | 5 V UART from the unit → 3.3 V for the ESP |

## CN105 UART wiring (rev 1 errata)

Mitsubishi CN105 pinout (viewed from the board):

| Pin | Signal | Direction (unit) |
| --- | --- | --- |
| 4 | TX | unit → ESP (ESP must **receive** here) |
| 5 | RX | ESP → unit (ESP must **transmit** here) |

**Rev 1 PCB bug:** pins 4 and 5 were swapped at the JST connector (ESP TX was routed to unit TX pin 4, ESP RX to unit RX pin 5). Symptom: yellow LED (Zigbee OK but CN105 silent), no UART traffic - no amount of firmware fixes this.

**Fix on assembled board:** cross **only** CN105 pins **4 ↔ 5** (swap the two UART wires on the cable or at `J1`). GND / 5 V unchanged.

Fixed in `mitsubishi2zigbee.kicad_pcb` for the next fabrication run.

![Schematic](schematic.png)

## Open in KiCad

```bash
kicad hardware/mitsubishi2zigbee.kicad_pro
```

Project-specific symbol and footprint libraries live in `libs/`.

## Layout

```
hardware/
├── README.md
├── schematic.png
├── mitsubishi2zigbee.kicad_pro
├── mitsubishi2zigbee.kicad_sch
├── mitsubishi2zigbee.kicad_pcb
├── fp-lib-table
├── sym-lib-table
└── libs/
    ├── JST.pretty/
    ├── Waveshare-ESP32.pretty/
    └── Waveshare-ESP32.kicad_sym
```
