#include "esp_wifi.h"
#include "SD.h"
#include "SPI.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/queue.h"

// --- Pin Configuration ---
#define SD_MISO  2
#define SD_MOSI  7
#define SD_SCK   6
#define SD_CS    10
#define LED_PIN  27

// --- Deduplication interval: one log entry per MAC per 30 seconds ---
#define MAC_DEDUP_INTERVAL_MS 30000

// --- Optimized log record: 12 bytes instead of 32 ---
// Raw MAC bytes instead of formatted char[18] string saves memory
// and avoids slow snprintf() inside the Wi-Fi callback
struct LogEntry {
  uint32_t time_ms;
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  frame_type;
  uint8_t  mac[6]; // Raw 6-byte MAC address
};

// --- Global Variables ---
SPIClass      spiSD(FSPI);
char          activeFileName[32];
volatile int  current_scanning_channel = 1; // volatile: written in loop(), read in callback
QueueHandle_t logQueue;                      // Thread-safe bridge between callback and loop()

// --- Static Deduplication Ring Buffer ---
// Fixed-size array instead of std::map to avoid heap fragmentation.
// Automatically overwrites the oldest entry when full (circular buffer).
struct DedupRecord {
  uint8_t  mac[6];
  uint32_t last_seen; // millis() timestamp of last log entry
};
#define DEDUP_MAX_DEVICES 512
static DedupRecord dedupBuffer[DEDUP_MAX_DEVICES];
static int         nextDedupIndex = 0;

// Check if MAC was seen recently. Updates timestamp if expired.
// Uses memcmp for fast raw byte comparison — no string operations needed.
bool isDuplicate(const uint8_t* macBytes, uint32_t now) {
  for (int i = 0; i < DEDUP_MAX_DEVICES; i++) {
    if (memcmp(dedupBuffer[i].mac, macBytes, 6) == 0) {
      if (now - dedupBuffer[i].last_seen < MAC_DEDUP_INTERVAL_MS) {
        return true; // Seen recently — suppress
      }
      // Interval expired — update timestamp and allow logging
      dedupBuffer[i].last_seen = now;
      return false;
    }
  }
  // New MAC — write into next slot in the ring buffer
  memcpy(dedupBuffer[nextDedupIndex].mac, macBytes, 6);
  dedupBuffer[nextDedupIndex].last_seen = now;
  nextDedupIndex = (nextDedupIndex + 1) % DEDUP_MAX_DEVICES;
  return false;
}

// --- Promiscuous Sniffer Callback ---
// Called from a high-priority Wi-Fi task.
// Rule: NO slow operations here (SD, Serial, snprintf) — only push to queue!
void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  // Level 1: allow only Management and Data frames, drop Control/Misc
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t *pkt       = (wifi_promiscuous_pkt_t *)buf;
  uint8_t                *payload   = pkt->payload;
  int8_t                  rssi      = pkt->rx_ctrl.rssi;
  uint8_t                 frameType = payload[0];

  // Level 2: filter by 802.11 frame subtype
  // 0x40 — Probe Request (client actively searching for networks)
  // 0x08 — Data
  // 0x88 — QoS Data
  if (frameType != 0x40 && frameType != 0x08 && frameType != 0x88) return;

  // Drop weak signals and null/broadcast MACs
  if (rssi <= -95 || payload[10] == 0x00) return;

  // Fill the log entry with raw bytes — no string formatting in ISR context
  LogEntry entry;
  entry.time_ms    = millis();
  entry.channel    = (uint8_t)current_scanning_channel;
  entry.rssi       = rssi;
  entry.frame_type = frameType;
  memcpy(entry.mac, &payload[10], 6); // Fast raw byte copy

  // Push to queue without blocking.
  // If queue is full — packet is silently dropped, which is acceptable.
  xQueueSendFromISR(logQueue, &entry, NULL);

  // Brief blue flash to indicate packet capture
  neopixelWrite(LED_PIN, 0, 0, 20);
}

// --- Helper: drain the queue and write a batch of entries to SD ---
// Key optimizations:
// 1. SD file opened ONCE per batch, not once per packet
// 2. Deduplication happens before any string formatting
// 3. Limited to 20 entries per call to prevent blocking channel hops
void flushQueueToSD() {
  LogEntry entry;
  int      processed  = 0;
  File     logFile;
  bool     isFileOpen = false;
  uint32_t now        = millis();

  while (processed < 20 && xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
    processed++;

    // Deduplication check using raw bytes — fast memcmp, no string allocation
    if (isDuplicate(entry.mac, now)) {
      continue; // Seen recently — skip without any SD or Serial activity
    }

    // Format MAC string only here, when we are certain this entry will be logged
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             entry.mac[0], entry.mac[1], entry.mac[2],
             entry.mac[3], entry.mac[4], entry.mac[5]);

    // Open SD file once for the entire batch, not on every packet
    if (!isFileOpen) {
      logFile    = SD.open(activeFileName, FILE_APPEND);
      isFileOpen = true;
    }

    if (logFile) {
      logFile.printf("%lu;%d;%d;%s\n",
                     entry.time_ms,
                     (int)entry.channel,
                     (int)entry.rssi,
                     macStr);
    }

    // Serial output is safe here — we are in the main task context
    Serial.printf("[Type: 0x%02X][CH %2d] RSSI: %3d | MAC: %s\r\n",
                  entry.frame_type,
                  (int)entry.channel,
                  (int)entry.rssi,
                  macStr);
  }

  // Close SD file once after the entire batch is written
  if (isFileOpen && logFile) {
    logFile.close();
  }

  // Turn off LED after processing the full batch
  neopixelWrite(LED_PIN, 0, 0, 0);
}

void setup() {
  Serial.begin(115200);
  delay(2000);       // Wait for Serial Monitor to connect on macOS/Windows
  Serial.flush();    // Clear bootloader garbage from the UART buffer
  Serial.println("\n--- ESP32-C5 PROBE SNIFFER START ---");

  pinMode(LED_PIN, OUTPUT);
  neopixelWrite(LED_PIN, 0, 0, 0);

  // Suppress verbose Wi-Fi driver logs
  esp_log_level_set("wifi", ESP_LOG_NONE);

  // Zero out dedup buffer — ensures clean state on boot
  memset(dedupBuffer, 0, sizeof(dedupBuffer));

  // Create the FreeRTOS queue — 128 slots
  // 128 * 12 bytes (LogEntry) = ~1.5KB RAM
  logQueue = xQueueCreate(128, sizeof(LogEntry));
  if (logQueue == NULL) {
    Serial.println("ERROR: Queue create FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0);
    while (true) { delay(1000); }
  }

  // Initialize SPI bus and mount SD card
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  delay(200); // Allow SPI bus voltage to stabilize

  Serial.print("Mounting SD Card...");
  if (!SD.begin(SD_CS, spiSD, 4000000)) {
    Serial.println(" FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0); // Red: SD mount error
    while (true) { delay(1000); }
  }
  Serial.println(" SUCCESS!");
  neopixelWrite(LED_PIN, 0, 150, 0); // Green: SD ready

  // Auto-increment log filename: log.csv -> log_1.csv -> log_2.csv ...
  int n = 0;
  strncpy(activeFileName, "/log.csv", sizeof(activeFileName));
  while (SD.exists(activeFileName)) {
    n++;
    snprintf(activeFileName, sizeof(activeFileName), "/log_%d.csv", n);
  }

  // Create log file and write CSV header row
  File logFile = SD.open(activeFileName, FILE_WRITE);
  if (logFile) {
    logFile.println("Time_ms;Channel;RSSI;MAC");
    logFile.close();
    Serial.printf("Logging to: %s\r\n", activeFileName);
  } else {
    Serial.println("ERROR: File create FAILED!");
    neopixelWrite(LED_PIN, 200, 0, 0);
    while (true) { delay(1000); }
  }

  neopixelWrite(LED_PIN, 0, 0, 0); // Turn off green LED

  // Initialize Wi-Fi in promiscuous (monitor) mode
  nvs_flash_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE); // Disable power save for maximum capture rate

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);

  // Discard any packets captured during setup() to keep Serial output clean
  xQueueReset(logQueue);

  Serial.println("Sniffing started!");
}

void loop() {
  // Channel list: 2.4 GHz non-overlapping + CH13, then full 5 GHz sweep
  // Covers standard, DFS, and upper bands used by iOS/macOS/Android devices
  static const uint8_t channels[] = {
    1, 6, 11, 13,           // 2.4 GHz (non-overlapping + EU channel 13)
    36, 40, 44, 48,         // 5 GHz lower band (most popular)
    52, 56, 60, 64,         // 5 GHz DFS band (commonly used by Apple devices)
    100, 108, 116, 132,     // 5 GHz middle band
    149, 153, 157, 161      // 5 GHz upper band (popular in US/CN devices)
  };
  static const int numChannels = sizeof(channels) / sizeof(channels[0]);

  // No lastPurge block needed — dedupBuffer is a fixed ring buffer
  // that automatically overwrites the oldest entry, requiring zero maintenance

  for (int i = 0; i < numChannels; i++) {
    current_scanning_channel = channels[i];
    esp_wifi_set_channel(current_scanning_channel, WIFI_SECOND_CHAN_NONE);

    // Dwell time: 150ms on 2.4 GHz, 250ms on 5 GHz.
    // Instead of a blocking delay(), actively drain the queue every 10ms
    // so no entries are lost and channel hops stay on schedule.
    uint32_t dwellMs  = (current_scanning_channel < 14) ? 150 : 250;
    uint32_t dwellEnd = millis() + dwellMs;

    while (millis() < dwellEnd) {
      flushQueueToSD(); // Max 20 entries per call — won't block the hop
      delay(10);
    }
  }
}