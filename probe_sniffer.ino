#include "esp_wifi.h"
#include "SD.h"
#include "SPI.h"
#include "nvs_flash.h"
#include "esp_log.h"

// --- Pin Configuration ---
#define SD_MISO  2
#define SD_MOSI  7
#define SD_SCK   6
#define SD_CS    10
#define LED_PIN  27

// --- Global Variables ---
SPIClass spiSD(FSPI);
char activeFileName[32]; 
int current_scanning_channel = 1;

// --- Sniffer Callback Function ---
void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != 0) return; // Skip if it's not a Management or Data packet

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint8_t *payload = pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi; 
  uint8_t frameType = payload[0];

  // Check if frame is a Probe Request (0x40) OR Data Frame (0x08) OR QoS Data (0x88)
  // This covers almost all active client devices in the air
  if ((frameType == 0x40 || frameType == 0x08 || frameType == 0x88) && payload[10] != 0x00 && rssi > -95) { // If the first MAC byte is not 00, proceed
    // Blue flash on packet capture
    neopixelWrite(LED_PIN, 0, 0, 20); 

    char macStr[18];
    // Transmitter MAC address in Management and Data frames typically starts at the 10th byte
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             payload[10], payload[11], payload[12], payload[13], payload[14], payload[15]);

    // Open file for appending data
    File logFile = SD.open(activeFileName, FILE_APPEND);
    if (logFile) {
      // Save data using ";" separator for easy import into Excel/Numbers
      logFile.printf("%lu;%d;%d;%s\n", millis(), current_scanning_channel, (int)pkt->rx_ctrl.rssi, macStr);
      logFile.close();
      
      // Console output for monitoring
      // 1. Frame Type (payload[0])
      // 2. Channel (current_scanning_channel)
      // 3. Signal Strength (pkt->rx_ctrl.rssi)
      // 4. MAC address (macStr)
      Serial.printf("[Type: 0x%02X][CH %d] RSSI: %d | MAC: %s\r\n", 
                    payload[0], 
                    current_scanning_channel, 
                    (int)pkt->rx_ctrl.rssi, 
                    macStr);
    }

    // Turn off LED
    neopixelWrite(LED_PIN, 0, 0, 0); 
  }
}

void setup() {
  Serial.begin(115200);
  // Short delay to allow the Serial Monitor to hook up on macOS/Windows
  delay(1500); 
  Serial.println("\n--- ESP32-C5 PROBE SNIFFER START ---");

  pinMode(LED_PIN, OUTPUT);

  // Mute redundant Wi-Fi system logs
  esp_log_level_set("wifi", ESP_LOG_NONE);

  // Initialize SPI for the SD card
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  Serial.print("Mounting SD Card...");
  // 16MHz frequency for fast file checking and writes
  delay(200); // Даем время шине стабилизироваться
  if (!SD.begin(SD_CS, spiSD, 4000000)) { 
    Serial.println(" FAILED!");
    // RED INDICATOR: SD Card mount error
    neopixelWrite(LED_PIN, 200, 0, 0); 
    while(true) { delay(1000); } 
  } 

  Serial.println(" SUCCESS!");
  // GREEN INDICATOR: Successful start (stays on while creating the file)
  neopixelWrite(LED_PIN, 0, 150, 0); 

  // Auto-increment filename (log.csv, log_1.csv, log_2.csv ...)
  int n = 0;
  strncpy(activeFileName, "/log.csv", sizeof(activeFileName));
  while (SD.exists(activeFileName)) {
    n++;
    snprintf(activeFileName, sizeof(activeFileName), "/log_%d.csv", n);
  }

  // Create the log file and write CSV headers
  File logFile = SD.open(activeFileName, FILE_WRITE);
  if (logFile) {
    logFile.println("Time_ms;Channel;RSSI;MAC");
    logFile.close();
    Serial.printf("Logging to: %s\r\n", activeFileName);
  }

  // Turn off the green LED before starting the scan
  neopixelWrite(LED_PIN, 0, 0, 0); 

  // Configure Wi-Fi in promiscuous (monitoring) mode
  nvs_flash_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  
  // Disable Wi-Fi power-saving mode for maximum capture rate
  esp_wifi_set_ps(WIFI_PS_NONE); 
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);
}

void loop() {
  // Priority channels + DFS + Extended 5GHz range
  uint8_t channels[] = {
    1, 6, 11, 13,             // 2.4 GHz (Standard non-overlapping + CH13)
    36, 40, 44, 48,           // 5 GHz (Lower band, highly popular)
    52, 56, 60, 64,           // 5 GHz (DFS band, commonly used by iOS/macOS)
    100, 108, 116, 132,       // 5 GHz (Middle band)
    149, 153, 157, 161        // 5 GHz (Upper band, popular in US/China models)
  };
  
  for (int i = 0; i < sizeof(channels); i++) {
    current_scanning_channel = channels[i];
    esp_wifi_set_channel(current_scanning_channel, WIFI_SECOND_CHAN_NONE);
    
    // Balanced channel dwell times: 150ms for 2.4GHz, 250ms for 5GHz
    if (current_scanning_channel < 14) {
      delay(150); 
    } else {
      delay(250); 
    }
  }
}