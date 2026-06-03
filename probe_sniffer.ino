#include "esp_wifi.h"
#include "SD.h"
#include "SPI.h"
#include "nvs_flash.h"
#include "esp_log.h"

// --- Конфигурация пинов ---
#define SD_MISO  2
#define SD_MOSI  7
#define SD_SCK   6
#define SD_CS    10
#define LED_PIN  27

// --- Глобальные переменные ---
SPIClass spiSD(FSPI);
char activeFileName[32]; 
int current_scanning_channel = 1;

// --- Функция обратного вызова сниффера ---
void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != 0) return; // Пропускаем, если это не Management/Data пакет

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint8_t *payload = pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi; 
  uint8_t frameType = payload[0];

  // Проверяем: это Probe Request (0x40) ИЛИ Data Frame (0x08) ИЛИ QoS Data (0x88) ИЛИ Beacon Frames (0x80)
  // Это покроет почти все активные устройства в эфире
  if ((frameType == 0x40 || frameType == 0x08 || frameType == 0x88) && payload[10] != 0x00 && rssi > -95) { // Если первый байт MAC не 00, идем дальше
    // Синяя вспышка при поимке пакета
    neopixelWrite(LED_PIN, 0, 0, 20); 

    char macStr[18];
    // MAC-адрес отправителя в Management и Data кадрах обычно начинается с 10-го байта
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             payload[10], payload[11], payload[12], payload[13], payload[14], payload[15]);

    // Открываем файл для дозаписи
    File logFile = SD.open(activeFileName, FILE_APPEND);
    if (logFile) {
      // Записываем данные с разделителем ";" для Excel/Numbers
      logFile.printf("%lu;%d;%d;%s\n", millis(), current_scanning_channel, (int)pkt->rx_ctrl.rssi, macStr);
      logFile.close();
      
      // Вывод в консоль для контроля
      // 1. Тип кадра (payload[0])
      // 2. Канал (current_scanning_channel)
      // 3. Уровень сигнала (pkt->rx_ctrl.rssi)
      // 4. MAC-адрес (macStr)
      Serial.printf("[Type: 0x%02X][CH %d] RSSI: %d | MAC: %s\r\n", 
                    payload[0], 
                    current_scanning_channel, 
                    (int)pkt->rx_ctrl.rssi, 
                    macStr);
    }

    // Гасим светодиод
    neopixelWrite(LED_PIN, 0, 0, 0); 
  }
}

void setup() {
  Serial.begin(115200);
  // Пауза, чтобы Serial Monitor на MacBook успел подцепиться
  delay(1500); 
  Serial.println("\n--- ESP32-C5 PROBE SNIFFER START ---");

  pinMode(LED_PIN, OUTPUT);

  // Глушим лишние системные логи Wi-Fi
  esp_log_level_set("wifi", ESP_LOG_NONE);

  // Инициализация SPI для SD-карты
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  Serial.print("Mounting SD Card...");
  // Частота 16МГц для быстрой записи и проверки файлов
  delay(200); // Даем время шине стабилизироваться
  if (!SD.begin(SD_CS, spiSD, 4000000)) { 
    Serial.println(" FAILED!");
    // КРАСНЫЙ ИНДИКАТОР: Ошибка монтирования карты
    neopixelWrite(LED_PIN, 200, 0, 0); 
    while(true) { delay(1000); } 
  } 

  Serial.println(" SUCCESS!");
  // ЗЕЛЕНЫЙ ИНДИКАТОР: Успешный старт (горит, пока создается файл)
  neopixelWrite(LED_PIN, 0, 150, 0); 

  // Автоматический подбор имени файла (log.csv, log_1.csv ...)
  int n = 0;
  strncpy(activeFileName, "/log.csv", sizeof(activeFileName));
  while (SD.exists(activeFileName)) {
    n++;
    snprintf(activeFileName, sizeof(activeFileName), "/log_%d.csv", n);
  }

  // Создание файла и запись заголовков
  File logFile = SD.open(activeFileName, FILE_WRITE);
  if (logFile) {
    logFile.println("Time_ms;Channel;RSSI;MAC");
    logFile.close();
    Serial.printf("Logging to: %s\r\n", activeFileName);
  }

  // Гасим зеленый перед началом сканирования
  neopixelWrite(LED_PIN, 0, 0, 0); 

  // Настройка Wi-Fi в режиме мониторинга
  nvs_flash_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  
  // Отключаем режим энергосбережения для максимальной скорости
  esp_wifi_set_ps(WIFI_PS_NONE); 
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);
}

void loop() {
  // Список приоритетных каналов 2.4GHz и основных 5GHz
  // uint8_t channels[] = {1, 6, 11, 13, 36, 40, 44, 48, 52, 56, 60, 64, 100, 108, 116, 149, 153, 157, 161};
  // Приоритетные + DFS + Расширенные 5ГГц
  uint8_t channels[] = {
    1, 6, 11, 13,             // 2.4 GHz (Основные + наш 13-й)
    36, 40, 44, 48,           // 5 GHz (Нижний диапазон, очень популярен)
    52, 56, 60, 64,           // 5 GHz (DFS диапазон, тут часто сидят iPhone/Mac)
    100, 108, 116, 132,            // 5 GHz (Средний диапазон)
    149, 153, 157, 161        // 5 GHz (Верхний диапазон, популярен в США/Китае)
  };
  
  for (int i = 0; i < sizeof(channels); i++) {
    current_scanning_channel = channels[i];
    esp_wifi_set_channel(current_scanning_channel, WIFI_SECOND_CHAN_NONE);
    
    // 300мс на 2.4GHz (быстрее) и 500мс на 5GHz (т.к. там реже шлют Probe)
    if (current_scanning_channel < 14) {
      delay(150); 
    } else {
      delay(250); 
    }
  }
}