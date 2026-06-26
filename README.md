# ESP32-C5 MAC Sniffer

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![Platform](https://img.shields.io/badge/platform-ESP32--C5-blue)
![Arduino](https://img.shields.io/badge/Arduino-ESP32%20Core%203.x-teal)
![Band](https://img.shields.io/badge/Wi--Fi-2.4%20%2F%205%20GHz-orange)
![BLE](https://img.shields.io/badge/BLE-5.0-purple)

> Built by me and Claude, with code review by Gemini and Codex.

ESP32-C5 based passive Wi-Fi + BLE scanner that captures MAC addresses from the air
and logs them to a MicroSD card as CSV files. Sweeps 2.4 GHz and 5 GHz Wi-Fi channels
and concurrently scans Bluetooth LE advertisements, filters duplicate devices, and runs
indefinitely with automatic log rotation.
Starts capturing automatically as soon as USB power is connected — no PC required
after flashing.

Created for Wi-Fi audience analytics research, as no lightweight plug-and-play
solution was found that starts capturing automatically without additional setup.

Pairs with [yandex-mac-converter](https://github.com/Grostl/yandex-mac-converter) —
a desktop tool that deduplicates and converts the captured CSV logs into the format
required by Yandex Audiences, with optional filtering by frame type, randomised MACs,
and device vendor.

## Responsible Use

This tool performs passive monitoring only — it does not transmit, inject packets,
or interfere with any network or device.

Always obtain explicit permission from the venue or property owner before deploying
this device. In many countries, capturing wireless data in public spaces without
consent may violate privacy laws such as GDPR (EU), CCPA (US), or equivalent
local regulations. The author takes no responsibility for unlawful use.

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-C5 dev board |
| Storage | MicroSD card module |
| LED | Built-in NeoPixel on GPIO 27 |

### Wiring

| SD Module Pin | ESP32-C5 GPIO |
|---|---|
| MISO | 2 |
| MOSI | 7 |
| SCK | 6 |
| CS | 10 |

> VCC and GND connections depend on your specific SD card module — check your module's datasheet.

> SD card must be formatted as **FAT32**. exFAT is not supported.

---

## Features

- Passive 2.4 GHz + 5 GHz Wi-Fi channel sweep (20 channels)
- Concurrent BLE 5.0 passive scan — captures public Bluetooth advertisements
- Captures five 802.11 frame subtypes + BLE advertisements — see [Captured Frame Types](#captured-frame-types) below
- 300 ms dwell on 2.4 GHz, 200 ms on 5 GHz (more weight where most clients are)
- Wi-Fi and BLE share the radio via ESP32-C5 SW coexistence (BLE at 30% duty cycle)
- Starts automatically on USB power — no PC needed
- Plug-and-play: insert SD card, apply power, done
- MAC deduplication — one entry per device per 30 seconds
- Filters multicast, broadcast, and randomised BLE addresses — only real unicast MACs logged
- Automatic log rotation at 500 000 entries per file
- NeoPixel LED status indicators

---

## Captured Frame Types

The sniffer records five 802.11 frame subtypes and BLE advertisements:

| Frame type | Hex | Source | Notes |
|---|---|---|---|
| Probe Request | `0x40` | Wi-Fi | Sent by devices scanning for networks. Modern OS randomise the MAC here |
| Data | `0x08` | Wi-Fi | Sent by devices connected to a network — real hardware MAC |
| QoS Data | `0x88` | Wi-Fi | Same as Data, QoS variant — real hardware MAC |
| Null | `0x48` | Wi-Fi | Power-save signalling from connected device — real hardware MAC |
| QoS Null | `0xC8` | Wi-Fi | Power-save signalling, QoS variant — real hardware MAC |
| BLE Advertisement | `0xBE` | BLE | Public-address advertisements only — real hardware MAC. Channel logged as `0` |

Data / Null / BLE frames yield the most reliable MACs for audience matching.
Probe Requests are logged for completeness and can be excluded in the companion converter.

---

## LED Indicators

| Color | Pattern | Meaning |
|---|---|---|
| Green | Brief flash | SD card mounted successfully, starting scan |
| Blue | Flash | MAC address captured |
| Red | Solid | SD card or Wi-Fi init error — check Serial Monitor at 115200 baud |

---

## Output Format

Log files are saved as `/log.csv`, `/log_1.csv`, `/log_2.csv` and so on.
Files use `;` as delimiter and open directly in Excel or Numbers.

| Column | Description |
|---|---|
| Time_ms | Milliseconds since boot |
| Channel | Wi-Fi channel number |
| RSSI | Signal strength in dBm |
| FrameType | 802.11 frame subtype (hex) |
| MAC | Device MAC address (`XX:XX:XX:XX:XX:XX`) |

---

## Building

Install [Arduino CLI](https://arduino.github.io/arduino-cli/) and the ESP32 board package, then:

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32c5:UploadSpeed=921600,CDCOnBoot=default,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,PSRAM=disabled" \
  --output-dir build/esp32.esp32.esp32c5 \
  .
```

Flash with:

```bash
arduino-cli upload \
  --fqbn "esp32:esp32:esp32c5" \
  --port /dev/cu.usbserial-* \
  --input-dir build/esp32.esp32.esp32c5
```

A pre-built `.bin` is available in [Releases](../../releases).

---

## Dependencies

- [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) 3.x
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 3.x — required for BLE scanning.
  Install via Arduino IDE Library Manager (search **NimBLE**) or add the ZIP from GitHub.
  ESP32-C5 uses NimBLE as its BLE stack; the bundled Bluedroid library is not supported on this chip.
- `SD.h` — included with ESP32 Arduino core
- `esp_wifi.h` — included with ESP32 Arduino core

---

## License

MIT — see [LICENSE](LICENSE)
