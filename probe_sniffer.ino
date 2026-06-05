#include "Arduino.h"
#include "esp_wifi.h"
#include "SD.h"
#include "SPI.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "string.h"

// --- Pin Configuration ---
#define SD_MISO  2
#define SD_MOSI  7
#define SD_SCK   6
#define SD_CS    10
#define LED_PIN  27

// --- Capture / Logging Settings ---
#define MAC_DEDUP_INTERVAL_MS  30000UL  // One log entry per MAC per 30 seconds
#define DEDUP_MAX_DEVICES      512      // Static ring buffer size (~5KB RAM)
#define LOG_QUEUE_SIZE         256      // FreeRTOS queue depth
#define LOG_BATCH_LIMIT        32       // Max SD writes per flushQueueToSD() call
#define MIN_RSSI_DBM           -95      // Drop frames weaker than this
#define LED_FLASH_MS           25       // Capture LED flash duration
#define MAX_LOG_ENTRIES        500000UL // Rotate to a new file after this many records

// --- Compact Log Record ---
// MAC stored as 6 raw bytes; formatted string built only when writing to CSV
struct LogEntry {
  uint32_t time_ms;
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  frame_type;
  uint8_t  mac[6];
};

// --- Deduplication Record ---
struct DedupRecord {
  uint8_t  mac[6];
  uint32_t last_seen;
};

// --- Global Variables ---
SPIClass          spiSD(FSPI);
char              activeFileName[32];
volatile uint8_t  currentScanningChannel = 1; // Written in loop(), read in callback
volatile uint32_t ledOnUntilMs = 0;           // Set in callback, consumed in loop()
QueueHandle_t     logQueue;

// Static dedup ring buffer — no heap allocations, no fragmentation
static DedupRecord dedupBuffer[DEDUP_MAX_DEVICES];
static uint16_t    nextDedupIndex = 0;

// Log rotation counter and file index.
// currentFileIndex is set once in setup() and incremented on each rotation —
// avoids expensive SD.exists() loop after many files have been created.
static uint32_t logEntryCount   = 0;
static int      currentFileIndex = 0;

// --- MAC Validation ---
// Rejects zero MACs, broadcast (FF:FF:...), and multicast (LSB of first byte = 1).
// All real client devices use unicast MACs where that bit is 0.
bool isUsableMac(const uint8_t mac[6]) {
  if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 &&
      mac[3] == 0 && mac[4] == 0 && mac[5] == 0) return false;
  if ((mac[0] & 0x01) != 0) return false; // Multicast/broadcast — not a real device
  return true;
}

// --- Deduplication ---
// Returns true if this MAC was logged recently.
// Prefers empty slots, then expired slots, then ring-overwrites.
bool isDuplicateMac(const uint8_t mac[6], uint32_t now) {
  int reusableSlot = -1;

  for (int i = 0; i < DEDUP_MAX_DEVICES; i++) {
    if (dedupBuffer[i].last_seen == 0) {
      if (reusableSlot < 0) reusableSlot = i;
      continue;
    }

    if (memcmp(dedupBuffer[i].mac, mac, 6) == 0) {
      if (now - dedupBuffer[i].last_seen < MAC_DEDUP_INTERVAL_MS) {
        return true; // Seen recently — suppress
      }
      dedupBuffer[i].last_seen = now; // Interval expired — allow and refresh
      return false;
    }

    if (reusableSlot < 0 && now - dedupBuffer[i].last_seen >= MAC_DEDUP_INTERVAL_MS) {
      reusableSlot = i;
    }
  }

  // New MAC — write into best available slot
  int slot = reusableSlot >= 0 ? reusableSlot : nextDedupIndex;
  memcpy(dedupBuffer[slot].mac, mac, 6);
  dedupBuffer[slot].last_seen = now;
  nextDedupIndex = (slot + 1) % DEDUP_MAX_DEVICES;
  return false;
}

// --- MAC Formatter ---
// Called only when a record is confirmed unique and will be written to CSV
void formatMac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// --- Log File Rotation ---
// Increments currentFileIndex instead of re-scanning SD with exists() —
// avoids multi-second freeze when hundreds of log files already exist.
// After rotation the current batch is always broken so the next batch
// starts cleanly at the top of the new file, right after the header row.
void rotateLogFile() {
  logEntryCount = 0;
  currentFileIndex++;

  snprintf(activeFileName, sizeof(activeFileName), "/log_%d.csv", currentFileIndex);

  File newFile = SD.open(activeFileName, FILE_WRITE);
  if (newFile) {
    newFile.println("Time_ms;Channel;RSSI;FrameType;MAC");
    newFile.close();
    Serial.printf("Log rotated: %s\r\n", activeFileName);
  } else {
    Serial.println("ERROR: log rotation failed — file create error");
    neopixelWrite(LED_PIN, 200, 0, 0);
  }
}

// --- LED Service ---
// The callback only sets ledOnUntilMs; actual neopixelWrite() runs from loop().
// This keeps the flash visible without blocking the Wi-Fi task.
void updateCaptureLed() {
  static bool ledIsOn = false;
  uint32_t now = millis();
  bool shouldBeOn = (int32_t)(ledOnUntilMs - now) > 0;

  if (shouldBeOn != ledIsOn) {
    ledIsOn = shouldBeOn;
    neopixelWrite(LED_PIN, 0, 0, ledIsOn ? 20 : 0);
  }
}

// --- ESP Error Helper ---
bool checkEsp(esp_err_t result, const char* operation) {
  if (result == ESP_OK) return true;
  Serial.printf("ERROR: %s failed: %s\r\n", operation, esp_err_to_name(result));
  neopixelWrite(LED_PIN, 200, 0, 0);
  return false;
}

// --- Promiscuous Sniffer Callback ---
// Runs in the Wi-Fi task context — keep it fast.
// No SD, no Serial, no snprintf. Filter, validate, enqueue.
void snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  // Level 1: Management and Data only; drop Control/Misc
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t* pkt     = (wifi_promiscuous_pkt_t*)buf;
  uint8_t*                payload = pkt->payload;

  // Guard against malformed/short frames before reading payload bytes
  if (pkt->rx_ctrl.sig_len < 16) return;

  int8_t rssi = pkt->rx_ctrl.rssi;
  if (rssi <= MIN_RSSI_DBM) return;

  // Level 2: 802.11 frame subtype
  // 0x40 — Probe Request, 0x08 — Data, 0x88 — QoS Data
  uint8_t frameType = payload[0];
  if (frameType != 0x40 && frameType != 0x08 && frameType != 0x88) return;

  // Transmitter MAC starts at byte offset 10
  const uint8_t* mac = &payload[10];
  if (!isUsableMac(mac)) return;

  LogEntry entry;
  entry.time_ms    = millis();
  // Prefer the channel reported in the packet itself; fall back to scan variable
  entry.channel    = pkt->rx_ctrl.channel ? pkt->rx_ctrl.channel : currentScanningChannel;
  entry.rssi       = rssi;
  entry.frame_type = frameType;
  memcpy(entry.mac, mac, 6);

  // Non-blocking enqueue — drop silently if queue is full
  xQueueSendFromISR(logQueue, &entry, NULL);

  // Signal the LED timer; loop() performs the actual neopixelWrite()
  ledOnUntilMs = entry.time_ms + LED_FLASH_MS;
}

// --- Queue Drain and SD Logger ---
// Runs in loop() context — SD and Serial are safe here.
// File opened once per batch to minimise slow SPI open/close cycles.
// On rotation: closes current file, calls rotateLogFile(), then breaks
// the batch immediately so the next batch starts cleanly in the new file.
void flushQueueToSD() {
  LogEntry entry;
  File     logFile;
  bool     isFileOpen = false;
  int      processed  = 0;
  uint32_t now        = millis();

  while (processed < LOG_BATCH_LIMIT && xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
    processed++;

    // Deduplicate before any string formatting or SD activity
    if (isDuplicateMac(entry.mac, now)) continue;

    char macStr[18];
    formatMac(entry.mac, macStr);

    // Open the file once for the whole batch, not once per packet
    if (!isFileOpen) {
      logFile    = SD.open(activeFileName, FILE_APPEND);
      isFileOpen = true;
    }

    if (logFile) {
      logFile.printf("%lu;%u;%d;0x%02X;%s\n",
                     (unsigned long)entry.time_ms,
                     (unsigned int)entry.channel,
                     (int)entry.rssi,
                     (unsigned int)entry.frame_type,
                     macStr);

      logEntryCount++;

      // Rotate when entry limit is reached.
      // Break immediately so remaining queue entries go into the new file
      // on the next flushQueueToSD() call — file handles never overlap.
      if (logEntryCount >= MAX_LOG_ENTRIES) {
        logFile.close();
        isFileOpen = false;
        rotateLogFile();
        break;
      }
    } else {
      Serial.println("ERROR: log file append failed");
      neopixelWrite(LED_PIN, 200, 0, 0);
      break;
    }

    Serial.printf("[Type: 0x%02X][CH %3u] RSSI: %4d | MAC: %s\r\n",
                  entry.frame_type,
                  (unsigned int)entry.channel,
                  (int)entry.rssi,
                  macStr);
  }

  // Single close after the entire batch
  if (isFileOpen && logFile) logFile.close();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.flush();
  Serial.println("\n--- ESP32-C5 MAC SNIFFER START ---");

  pinMode(LED_PIN, OUTPUT);
  neopixelWrite(LED_PIN, 0, 0, 0);

  // Suppress verbose Wi-Fi driver output
  esp_log_level_set("wifi", ESP_LOG_NONE);

  // Zero out dedup buffer for a clean boot state
  memset(dedupBuffer, 0, sizeof(dedupBuffer));

  // Create FreeRTOS queue — 256 * 12 bytes = ~3KB RAM
  logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(LogEntry));
  if (logQueue == NULL) {
    Serial.println("ERROR: Queue create FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0);
    while (true) delay(1000);
  }

  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(200); // Allow SPI bus voltage to stabilize

  Serial.print("Mounting SD Card...");
  if (!SD.begin(SD_CS, spiSD, 4000000)) {
    Serial.println(" FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0);
    while (true) delay(1000);
  }
  Serial.println(" SUCCESS!");
  neopixelWrite(LED_PIN, 0, 150, 0); // Green: SD ready

  // Find the first free filename and remember the index for fast rotation later.
  // This SD.exists() loop runs only once at boot — rotation uses currentFileIndex++.
  int n = 0;
  strncpy(activeFileName, "/log.csv", sizeof(activeFileName));
  activeFileName[sizeof(activeFileName) - 1] = '\0';
  while (SD.exists(activeFileName)) {
    n++;
    snprintf(activeFileName, sizeof(activeFileName), "/log_%d.csv", n);
  }
  currentFileIndex = n; // Anchor for rotation — no SD.exists() needed after this

  File logFile = SD.open(activeFileName, FILE_WRITE);
  if (!logFile) {
    Serial.println("ERROR: File create FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0);
    while (true) delay(1000);
  }
  logFile.println("Time_ms;Channel;RSSI;FrameType;MAC");
  logFile.close();
  Serial.printf("Logging to: %s\r\n", activeFileName);

  neopixelWrite(LED_PIN, 0, 0, 0); // Turn off green LED

  // NVS is required by the Wi-Fi driver.
  // Erase and reinit if partition is full or version mismatch detected.
  esp_err_t nvsResult = nvs_flash_init();
  if (nvsResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    delay(100);
    nvsResult = nvs_flash_init();
  }
  if (!checkEsp(nvsResult, "nvs_flash_init")) while (true) delay(1000);

  // Clean up any leftover Wi-Fi state from previous session
  esp_wifi_stop();
  esp_wifi_deinit();

  // Initialize Wi-Fi in promiscuous (monitor) mode
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (!checkEsp(esp_wifi_init(&cfg),                           "esp_wifi_init"))         while (true) delay(1000);
  if (!checkEsp(esp_wifi_set_storage(WIFI_STORAGE_RAM),        "esp_wifi_set_storage"))  while (true) delay(1000);
  if (!checkEsp(esp_wifi_set_mode(WIFI_MODE_NULL),             "esp_wifi_set_mode"))     while (true) delay(1000);
  if (!checkEsp(esp_wifi_start(),                              "esp_wifi_start"))        while (true) delay(1000);
  if (!checkEsp(esp_wifi_set_ps(WIFI_PS_NONE),                 "esp_wifi_set_ps"))       while (true) delay(1000);
  if (!checkEsp(esp_wifi_set_promiscuous_rx_cb(&snifferCallback), "set_promiscuous_cb")) while (true) delay(1000);
  if (!checkEsp(esp_wifi_set_promiscuous(true),                "set_promiscuous"))       while (true) delay(1000);

  // Discard packets captured during setup() to keep Serial output clean
  xQueueReset(logQueue);
  Serial.println("Sniffing started!");
}

void loop() {
  // 2.4 GHz non-overlapping + CH13, then full 5 GHz sweep
  static const uint8_t channels[] = {
    1, 6, 11, 13,           // 2.4 GHz (non-overlapping + EU channel 13)
    36, 40, 44, 48,         // 5 GHz lower band (most popular)
    52, 56, 60, 64,         // 5 GHz DFS band (commonly used by Apple devices)
    100, 108, 116, 132,     // 5 GHz middle band
    149, 153, 157, 161      // 5 GHz upper band (popular in US/CN devices)
  };
  static const int numChannels = sizeof(channels) / sizeof(channels[0]);

  for (int i = 0; i < numChannels; i++) {
    currentScanningChannel = channels[i];
    esp_err_t result = esp_wifi_set_channel(currentScanningChannel, WIFI_SECOND_CHAN_NONE);

    // Some 5 GHz channels may be blocked by regional regulations — skip gracefully
    if (result != ESP_OK) {
      Serial.printf("WARN: channel %u skipped: %s\r\n",
                    (unsigned int)currentScanningChannel,
                    esp_err_to_name(result));
      continue;
    }

    // Dwell time: 150ms on 2.4 GHz, 250ms on 5 GHz
    // Active queue drain every 10ms instead of a blocking delay()
    uint32_t dwellMs  = currentScanningChannel < 14 ? 150 : 250;
    uint32_t dwellEnd = millis() + dwellMs;

    while ((int32_t)(dwellEnd - millis()) > 0) {
      flushQueueToSD();
      updateCaptureLed();
      delay(10);
    }
  }
}