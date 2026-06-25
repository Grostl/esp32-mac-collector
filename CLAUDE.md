# CLAUDE.md — инструкции для Claude

## Что делает этот проект

Пассивный Wi-Fi снифер на ESP32-C5. Захватывает MAC-адреса устройств из эфира и пишет их в CSV на MicroSD карту. Создан для сбора данных для Яндекс Аудиторий — в паре с [yandex-mac-converter](https://github.com/Grostl/yandex-mac-converter).

## Структура репозитория

```
probe_sniffer.ino                        — весь код (один файл)
build/esp32.esp32.esp32c5/
  probe_sniffer.ino.bin                  — скомпилированная прошивка
  probe_sniffer.ino.merged.bin           — merged bin для прошивки через esptool
  build.options.json                     — параметры последней компиляции
```

## Сборка

Arduino CLI, плата ESP32-C5:

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32c5:UploadSpeed=921600,CDCOnBoot=default,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,PSRAM=disabled,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default" \
  --output-dir build/esp32.esp32.esp32c5 \
  .
```

Результат коммитится в репозиторий и прикладывается к GitHub Release.

## Формат выходного CSV

Разделитель `;`, кодировка UTF-8:

```
Time_ms;Channel;RSSI;FrameType;MAC
3889;1;-55;0x88;50:5B:C2:59:86:F2
```

Заголовок `Time_ms;Channel;RSSI;FrameType;MAC` используется companion-конвертером для автоопределения формата ESP32 CSV.

## Захватываемые типы фреймов

| Hex | Тип | MAC реальный? |
|-----|-----|---------------|
| `0x40` | Probe Request | Нет — современные устройства рандомизируют MAC при сканировании |
| `0x08` | Data | Да |
| `0x88` | QoS Data | Да |
| `0x48` | Null (power-save) | Да |
| `0xC8` | QoS Null (power-save) | Да |

Probe Request логируются для полноты — фильтрация делается на стороне конвертера.

## Ключевые детали реализации

- **Callback снифера** (`snifferCallback`) работает в контексте Wi-Fi задачи — там запрещены SD, Serial, snprintf. Только фильтрация и `xQueueSendFromISR`.
- **Дедупликация** — статический кольцевой буфер `DedupRecord[512]`, один MAC логируется не чаще раза в 30 секунд.
- **Запись на SD** (`flushQueueToSD`) — в контексте `loop()`, файл открывается один раз на весь батч (до 32 записей), затем закрывается. Это минимизирует медленные SPI-операции open/close.
- **Ротация лога** — при достижении 500 000 записей создаётся следующий файл (`/log.csv` → `/log_1.csv` → ...).
- **Обход каналов** — 2.4 ГГц: каналы 1, 6, 11, 13 по 300 мс; 5 ГГц: 16 каналов по 200 мс. DFS-каналы, заблокированные регулятором, пропускаются gracefully.
- **LED** — callback только выставляет `ledOnUntilMs`, реальный `neopixelWrite` вызывается из `loop()` чтобы не блокировать Wi-Fi задачу.
- **Валидация MAC** — отбрасываются нулевые, broadcast (FF:FF:...) и multicast (LSB первого байта = 1).

## Пины

| Функция | GPIO |
|---------|------|
| SD MISO | 2 |
| SD MOSI | 7 |
| SD SCK | 6 |
| SD CS | 10 |
| NeoPixel LED | 27 |
