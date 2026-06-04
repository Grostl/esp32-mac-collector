#include "esp_wifi.h"
#include "SD.h"
#include "SPI.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "map"
#include "string"

// --- Pin Configuration ---
#define SD_MISO  2
#define SD_MOSI  7
#define SD_SCK   6
#define SD_CS    10
#define LED_PIN  27

// --- Deduplication interval: one log entry per MAC per 30 seconds ---
#define MAC_DEDUP_INTERVAL_MS 30000

// --- Single log record structure ---
struct LogEntry {
  uint32_t time_ms;
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  frame_type;
  char     mac[18];
};

// --- Global Variables ---
SPIClass      spiSD(FSPI);
char          activeFileName[32];
volatile int  current_scanning_channel = 1; // volatile: written in loop(), read in callback
QueueHandle_t logQueue;                      // thread-safe bridge between callback and loop()

// MAC address -> last seen timestamp (ms)
// Used to suppress duplicate entries for the same device
std::map<std::string, uint32_t> seenMacs;

// --- Promiscuous Sniffer Callback ---
// Called from a high-priority Wi-Fi task.
// Rule: NO slow operations here (SD, Serial) — only push to queue!
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

  // Fill the log entry struct
  // Transmitter MAC address starts at byte offset 10 in Mgmt/Data frames
  LogEntry entry;
  entry.time_ms    = millis();
  entry.channel    = (uint8_t)current_scanning_channel;
  entry.rssi       = rssi;
  entry.frame_type = frameType;
  snprintf(entry.mac, sizeof(entry.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           payload[10], payload[11], payload[12],
           payload[13], payload[14], payload[15]);

  // Push to queue without blocking.
  // If queue is full — packet is silently dropped, which is acceptable.
  xQueueSendFromISR(logQueue, &entry, NULL);

  // Brief blue flash to indicate packet capture
  neopixelWrite(LED_PIN, 0, 0, 20);
}

// --- Helper: drain the queue and write entries to SD ---
// Limited to 20 entries per call to prevent blocking channel hops.
// Remaining entries will be processed on the next call.
void flushQueueToSD() {
  LogEntry entry;
  int processed = 0;

  while (processed < 20 && xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
    processed++;
    std::string mac(entry.mac);
    uint32_t now = millis();

    // Deduplication check: skip if this MAC was logged recently
    auto it = seenMacs.find(mac);
    if (it != seenMacs.end() && (now - it->second) < MAC_DEDUP_INTERVAL_MS) {
      neopixelWrite(LED_PIN, 0, 0, 0);
      continue; // Too soon — drop this entry
    }

    // New or expired MAC — update the timestamp and log it
    seenMacs[mac] = now;

    // Safe to use SD here — we are in the main task context, not the Wi-Fi callback
    File logFile = SD.open(activeFileName, FILE_APPEND);
    if (logFile) {
      logFile.printf("%lu;%d;%d;%s\n",
                     entry.time_ms,
                     (int)entry.channel,
                     (int)entry.rssi,
                     entry.mac);
      logFile.close();
    }

    // Serial output is also safe here
    Serial.printf("[Type: 0x%02X][CH %2d] RSSI: %3d | MAC: %s\r\n",
                  entry.frame_type,
                  (int)entry.channel,
                  (int)entry.rssi,
                  entry.mac);

    // Turn off LED after successful write
    neopixelWrite(LED_PIN, 0, 0, 0);
  }
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

  // Create the FreeRTOS queue — 128 slots, ~3KB RAM total
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

  // Clear any packets captured during setup before entering the main loop
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

  // Periodically purge expired entries from the dedup map (~every 60s).
  // Prevents unbounded RAM growth in long-running sessions.
  static uint32_t lastPurge = 0;
  if (millis() - lastPurge > 60000) {
    lastPurge = millis();
    uint32_t now = millis();
    for (auto it = seenMacs.begin(); it != seenMacs.end(); ) {
      if (now - it->second > MAC_DEDUP_INTERVAL_MS) {
        it = seenMacs.erase(it); // Remove stale entry
      } else {
        ++it;
      }
    }
  }

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