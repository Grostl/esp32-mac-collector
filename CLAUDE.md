# CLAUDE.md

## What this project does

Passive Wi-Fi sniffer on ESP32-C5. Captures MAC addresses from the air and writes
them to CSV files on a MicroSD card. Built for audience data collection for Yandex
Audiences — pairs with [yandex-mac-converter](https://github.com/Grostl/yandex-mac-converter).

## Repository structure

```
probe_sniffer.ino                        — full source (single file)
build/esp32.esp32.esp32c5/
  probe_sniffer.ino.bin                  — compiled firmware
  probe_sniffer.ino.merged.bin           — merged binary for esptool flashing
  build.options.json                     — build parameters from last compile
```

## Build

Arduino CLI, ESP32-C5 board:

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32c5:UploadSpeed=921600,CDCOnBoot=default,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,PSRAM=disabled,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default" \
  --output-dir build/esp32.esp32.esp32c5 \
  .
```

The resulting `.bin` files are committed to the repository and attached to GitHub Releases.

## Output CSV format

Delimiter `;`, UTF-8 encoding:

```
Time_ms;Channel;RSSI;FrameType;MAC
3889;1;-55;0x88;50:5B:C2:59:86:F2
```

The header `Time_ms;Channel;RSSI;FrameType;MAC` is used by the companion converter
to auto-detect the ESP32 CSV format and parse frame types per row.

## Captured frame types

| Hex | Type | Real MAC? |
|-----|------|-----------|
| `0x40` | Probe Request | No — modern OS (iOS 14+, Android 10+) randomise the MAC when scanning |
| `0x08` | Data | Yes |
| `0x88` | QoS Data | Yes |
| `0x48` | Null (power-save) | Yes |
| `0xC8` | QoS Null (power-save) | Yes |
| `0xBE` | BLE Advertisement | Yes — only public (non-random) BLE MACs are logged |

Probe Requests are logged for completeness; filtering is handled in the companion converter.
BLE entries use channel `0` (BLE advertising runs on channels 37/38/39, not Wi-Fi channels).

## Key implementation details

- **Wi-Fi sniffer callback** (`snifferCallback`) runs in the Wi-Fi task context — no SD,
  Serial, or snprintf allowed there. Only filtering and `xQueueSendFromISR`.
- **BLE sniffer** (`BLECallback`) uses NimBLE-Arduino 3.x (`NimBLEScanCallbacks`). Runs in
  the NimBLE task context — `xQueueSend` (not FromISR) is used. Only public-address
  advertisements are captured; random/resolvable BLE MACs are dropped. NimBLE stores
  address bytes LSB-first; they are reversed to match Wi-Fi MAC byte order before logging.
- **BLE coexistence** — ESP32-C5 has one shared radio for Wi-Fi and BLE. `NimBLEDevice::init()`
  must be called before Wi-Fi init so the SW coexistence arbiter is in place. BLE scan duty
  cycle is 30% (interval=160×0.625 ms, window=48×0.625 ms) to leave headroom for Wi-Fi.
  Wi-Fi loses ~30% of its airtime to BLE, but this is acceptable for deduplication (one MAC
  per 30 s window) since devices send probe requests far more frequently than once per 30 s.
- **Deduplication** — static ring buffer `DedupRecord[512]`, one MAC logged at most once
  per 30 seconds. Shared between Wi-Fi and BLE MACs.
- **SD writes** (`flushQueueToSD`) — runs in `loop()` context. The file is opened once
  per batch (up to 32 entries) and closed after, minimising slow SPI open/close cycles.
- **Log rotation** — a new file is created every 500 000 entries (`/log.csv` → `/log_1.csv` → …).
- **Channel hopping** — 2.4 GHz: channels 1, 6, 11, 13 at 300 ms each; 5 GHz: 16 channels
  at 200 ms each. DFS channels blocked by regional regulations are skipped gracefully.
- **LED** — callbacks only set `ledOnUntilMs`; the actual `neopixelWrite` is called
  from `loop()` to avoid blocking Wi-Fi or BLE tasks.
- **MAC validation** — zero MACs, broadcast (FF:FF:…), and multicast (LSB of first byte = 1)
  are all dropped before enqueueing.

## Dependencies

- [Arduino ESP32 core](https://github.com/espressif/arduino-esp32) 3.x
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 3.x — install via Arduino IDE
  Library Manager (search "NimBLE") or add ZIP from GitHub. Required for BLE scanning;
  ESP32-C5 does not support the Bluedroid stack.

## Pin assignments

| Function | GPIO |
|----------|------|
| SD MISO | 2 |
| SD MOSI | 7 |
| SD SCK | 6 |
| SD CS | 10 |
| NeoPixel LED | 27 |
