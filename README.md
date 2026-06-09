# ESP32-C5 MAC Sniffer

> Built by me and Claude, with code review by Gemini and Codex.

ESP32-C5 based passive Wi-Fi scanner that captures MAC addresses from the air
and logs them to a MicroSD card as CSV files. Sweeps 2.4 GHz and 5 GHz channels,
filters duplicate devices, and runs indefinitely with automatic log rotation.
Starts capturing automatically as soon as USB power is connected — no PC required
after flashing.

Created for Wi-Fi audience analytics research, as no lightweight plug-and-play
solution was found that starts capturing automatically without additional setup.

## Responsible Use

This tool performs passive monitoring only — it does not transmit, inject packets,
or interfere with any network or device.

Always obtain explicit permission from the venue or property owner before deploying
this device. In many countries, capturing wireless data in public spaces without
consent may violate privacy laws such as GDPR (EU), CCPA (US), or equivalent
local regulations. The author take no responsibility for unlawful use.

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

- Passive 2.4 GHz + 5 GHz channel sweep (20 channels)
- Starts automatically on USB power — no PC needed
- Plug-and-play: insert SD card, apply power, done
- MAC deduplication — one entry per device per 30 seconds
- Filters multicast and broadcast — only real devices logged
- Automatic log rotation at 500 000 entries per file
- NeoPixel LED status indicators

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
| Channel | Wi-Fi channel |
| RSSI | Signal strength in dBm |
| FrameType | 802.11 frame subtype |
| MAC | Device MAC address |

---

## Dependencies

- [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) 3.x
- `SD.h` — included with ESP32 Arduino core
- `esp_wifi.h` — included with ESP32 Arduino core

---

## License

MIT — see [LICENSE](LICENSE)
