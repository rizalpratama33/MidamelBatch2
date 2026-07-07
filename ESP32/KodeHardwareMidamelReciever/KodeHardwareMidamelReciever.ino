#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <ArduinoJson.h>
#include <LittleFS.h> 
#include <time.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

/* ================= RTC CONFIG ================= */
RTC_DS3231 rtc;

/* ================= WIFI ================= */
#define WIFI_SSID "Kadus puhun"
#define WIFI_PASSWORD "1234567899"

/* ================= FIREBASE ================= */
#define FIREBASE_HOST "https://bell-visual-midamel-default-rtdb.firebaseio.com/devices/bel_sekolah_1"
#define FIREBASE_JADWAL FIREBASE_HOST "/jadwal"
#define FIREBASE_CONFIG FIREBASE_HOST "/config/version.json"
#define FIREBASE_STATUS FIREBASE_HOST "/status.json"
#define FIREBASE_OVERRIDE FIREBASE_HOST "/manual_override.json"

/* ================= PIN LORA (ESP32-S3) ================= */
#define NSS 5
#define RST 14
#define DI0 4 

/* ================= PIN PANEL KONTROL ================= */
#define SUBMIT_LED 6
#define SUBMIT_BUTTON 18
#define BELL_PIN 1  
#define LED_PIN 2   

// TK, SD, SMP, SMA pins tetap sesuai aslinya...
#define BUTTON_1 19
#define LED_GRN_1 48
#define LED_YLW_1 47
#define LED_RED_1 20
#define BUTTON_2 35 
#define LED_GRN_2 38
#define LED_YLW_2 37
#define LED_RED_2 36
#define BUTTON_3 39 
#define LED_GRN_3 42
#define LED_YLW_3 41
#define LED_RED_3 40
#define BUTTON_4 3 
#define LED_GRN_4 15
#define LED_YLW_4 16
#define LED_RED_4 17

#define SDA_PIN 8
#define SCL_PIN 9
LiquidCrystal_I2C lcd(0x27, 20, 4);

/* ================= VARIABEL GLOBAL ================= */
const char* HARI[7] = {"senin","selasa","rabu","kamis","jumat","sabtu","minggu"};
int counter[4] = {0, 0, 0, 0}; 
bool isIotActive[4] = {false, false, false, false};
const char* JENJANG_NAMES[4] = {"tk", "sd", "smp", "sma"};

unsigned long lastFirebaseCheck = 0;
unsigned long lastStatusUpdate = 0;
int lastMinuteTrigger = -1;

unsigned long bellEndTime = 0; 
volatile bool bellActive = false;
SemaphoreHandle_t bellMutex; 
SemaphoreHandle_t loraMutex; 

void initHardware() {
  Serial.println("[SETUP] Inisialisasi Hardware...");
  pinMode(BELL_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SUBMIT_LED, OUTPUT);
  digitalWrite(BELL_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(SUBMIT_LED, LOW);

  bellMutex = xSemaphoreCreateMutex();
  loraMutex = xSemaphoreCreateMutex();
}

void sendLoRa(int a, int b, int c, int d) {
  if (xSemaphoreTake(loraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    digitalWrite(SUBMIT_LED, HIGH);
    String packet = String(a % 5) + ";" + String(b % 5) + ";" + String(c % 5) + ";" + String(d % 5) + ";";
    Serial.printf("[LoRa] Mengirim Paket: %s...", packet.c_str());
    LoRa.beginPacket();
    LoRa.print(packet);
    if (LoRa.endPacket()) {
      Serial.println(" BERHASIL.");
    } else {
      Serial.println(" GAGAL.");
    }
    digitalWrite(SUBMIT_LED, LOW);
    xSemaphoreGive(loraMutex);
  }
}

void updateLEDs(int idx, int val) {
  int mod = val % 5;
  int pins[4][3] = {
    {LED_GRN_1, LED_YLW_1, LED_RED_1},
    {LED_GRN_2, LED_YLW_2, LED_RED_2},
    {LED_GRN_3, LED_YLW_3, LED_RED_3},
    {LED_GRN_4, LED_YLW_4, LED_RED_4}
  };
  digitalWrite(pins[idx][0], (mod == 3 || mod == 4) ? HIGH : LOW);
  digitalWrite(pins[idx][1], (mod == 2 || mod == 4) ? HIGH : LOW);
  digitalWrite(pins[idx][2], (mod == 1 || mod == 4) ? HIGH : LOW);
}

String getModeName(int val) {
  int mod = val % 5;
  if (mod == 1) return "MERAH   ";
  if (mod == 2) return "KUNING  ";
  if (mod == 3) return "HIJAU   ";
  if (mod == 4) return "DARURAT ";
  return "OFF     ";
}

void displayLCD() {
  if (bellActive) return; 
  for (int i = 0; i < 4; i++) {
    lcd.setCursor(0, i);
    String label = String(JENJANG_NAMES[i]);
    label.toUpperCase();
    lcd.print(label + ": " + getModeName(counter[i]));
  }
}

void startBell(uint16_t dur, String label) {
  if (xSemaphoreTake(bellMutex, portMAX_DELAY) == pdTRUE) {
    if (bellActive) {
      xSemaphoreGive(bellMutex);
      return;
    }
    bellActive = true;
    bellEndTime = millis() + (dur * 1000); 

    digitalWrite(BELL_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH); 
    digitalWrite(SUBMIT_LED, HIGH);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(">>> BEL AKTIF <<<");
    lcd.setCursor(0, 1);
    lcd.print(label.substring(0, 20));

    Serial.printf("\n🔔 [BELL START] %s | Durasi: %d detik\n", label.c_str(), dur);
    xSemaphoreGive(bellMutex);
  }
}

void handleBellStop() {
  if (bellActive && millis() >= bellEndTime) {
    if (xSemaphoreTake(bellMutex, portMAX_DELAY) == pdTRUE) {
      digitalWrite(BELL_PIN, LOW);
      digitalWrite(LED_PIN, LOW); 
      digitalWrite(SUBMIT_LED, LOW);
      bellActive = false;
      Serial.println("🔕 [BELL STOP] Bel telah berhenti.");
      lcd.clear();
      displayLCD(); 
      xSemaphoreGive(bellMutex);
    }
  }
}

String readFile(String filename) {
  File file = LittleFS.open("/" + filename + ".json", "r");
  if (!file) {
    Serial.printf("[FS] Gagal membuka file: %s.json\n", filename.c_str());
    return "";
  }
  String content = file.readString();
  file.close();
  return content;
}

bool writeFile(String filename, String content) {
  File file = LittleFS.open("/" + filename + ".json", "w");
  if (!file) {
    Serial.printf("[FS] Gagal menulis file: %s.json\n", filename.c_str());
    return false;
  }
  file.print(content);
  file.close();
  Serial.printf("[FS] File %s.json berhasil diperbarui.\n", filename.c_str());
  return true;
}

bool httpGET(String url, String &out) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); 
  http.setTimeout(10000); 
  
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == 200) {
      out = http.getString();
      http.end();
      return true;
    }
    Serial.printf("[HTTP] GET Error: %d\n", code);
    http.end();
  }
  return false;
}

bool httpPUT(String url, String payload) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setTimeout(10000);
  
  if (http.begin(client, url)) {
    int code = http.PUT(payload);
    http.end();
    return code == 200;
  }
  return false;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  Serial.printf("\n[WIFI] Menghubungkan ke: %s\n", WIFI_SSID);
  lcd.clear();
  lcd.print("WiFi Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.clear();
    lcd.print("WiFi Online");
  } else {
    Serial.println("\n[WIFI] Gagal terhubung (Timeout).");
    lcd.clear();
    lcd.print("WiFi Offline");
  }
}

void sendStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  JsonDocument doc;
  doc["is_online"] = true;
  time_t now;
  time(&now);
  doc["last_seen"] = (long long)now * 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  String payload;
  serializeJson(doc, payload);
  if(httpPUT(FIREBASE_STATUS, payload)) {
    Serial.println("[FB] Status online diperbarui.");
  }
}

void taskManualOverride(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String payload;
      if (httpGET(FIREBASE_OVERRIDE, payload)) {
        if (payload != "null" && payload.length() > 5) {
          JsonDocument doc;
          DeserializationError error = deserializeJson(doc, payload);
          if (!error && doc["trigger"] == true) {
            String target = doc["target"] | "Manual";
            Serial.printf("[OVERRIDE] Perintah manual dari Cloud terdeteksi: %s\n", target.c_str());
            
            startBell(10, "Manual Cloud: " + target);
            for(int k=0; k<3; k++) {
               sendLoRa(4, 4, 4, 4);
               vTaskDelay(300 / portTICK_PERIOD_MS); 
            }
            
            JsonDocument resetDoc;
            resetDoc["trigger"] = false;
            resetDoc["target"] = target;
            String response;
            serializeJson(resetDoc, response);
            httpPUT(FIREBASE_OVERRIDE, response);
          }
        }
      }
    }
    vTaskDelay(3000 / portTICK_PERIOD_MS); 
  }
}

void syncTime() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[TIME] Sinkronisasi NTP...");
    configTime(7 * 3600, 0, "pool.ntp.org", "id.pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
      Serial.printf("[TIME] NTP Berhasil: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      return;
    }
  }
  
  DateTime nowRTC = rtc.now();
  struct tm t_rtc;
  t_rtc.tm_year = nowRTC.year() - 1900;
  t_rtc.tm_mon = nowRTC.month() - 1;
  t_rtc.tm_mday = nowRTC.day();
  t_rtc.tm_hour = nowRTC.hour();
  t_rtc.tm_min = nowRTC.minute();
  t_rtc.tm_sec = nowRTC.second();
  time_t t = mktime(&t_rtc);
  struct timeval tv = { .tv_sec = t };
  settimeofday(&tv, NULL);
  Serial.printf("[TIME] Offline. Menggunakan RTC: %02d:%02d:%02d\n", nowRTC.hour(), nowRTC.minute(), nowRTC.second());
}

String getHariSekarang() {
  struct tm t;
  if (!getLocalTime(&t)) return "senin";
  int w = t.tm_wday; 
  if (w == 0) w = 7; 
  return String(HARI[w - 1]);
}

void downloadAllDays(int newVer) {
  Serial.printf("[SYNC] Mendownload Jadwal Baru (Versi: %d)...\n", newVer);
  for (int i = 0; i < 7; i++) {
    String payload;
    String url = String(FIREBASE_JADWAL) + "/" + HARI[i] + ".json";
    if (httpGET(url, payload) && payload != "null") {
      writeFile(HARI[i], payload);
      Serial.printf("  - %s: Selesai\n", HARI[i]);
    }
    yield();
  }
  writeFile("version", String(newVer));
  Serial.println("[SYNC] Sinkronisasi Jadwal Selesai.");
}

void checkAndRun() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  int currentTotalMin = t.tm_hour * 60 + t.tm_min;
  if (currentTotalMin == lastMinuteTrigger) return;

  String hari = getHariSekarang();
  String json = readFile(hari);
  if (json.length() < 5) return;

  JsonDocument doc;
  if(deserializeJson(doc, json)) return;

  lastMinuteTrigger = currentTotalMin;
  bool anyTriggered = false;
  int maxDur = 0;
  String labels = "";

  for (int i = 0; i < 4; i++) {
    const char* j = JENJANG_NAMES[i];
    if (doc.containsKey(j)) {
      JsonVariant data = doc[j];
      auto process = [&](JsonObject item) {
        if ((item["aktif"] | false) && (item["jam"] == t.tm_hour) && (item["menit"] == t.tm_min)) {
          counter[i] = item["mode"] | 3;
          isIotActive[i] = true;
          anyTriggered = true;
          int d = item["durasi"] | 5;
          if (d > maxDur) maxDur = d;
          if (labels != "") labels += ", ";
          labels += String(j);
        }
      };
      if (data.is<JsonArray>()) {
        for (JsonObject it : data.as<JsonArray>()) process(it);
      } else if (data.is<JsonObject>()) {
        for (JsonPair kv : data.as<JsonObject>()) process(kv.value().as<JsonObject>());
      }
    }
  }

  if (anyTriggered) {
    Serial.printf("[SCHEDULE] Waktu Bel Tiba! Jenjang: %s\n", labels.c_str());
    for(int k=0; k<3; k++) { sendLoRa(counter[0], counter[1], counter[2], counter[3]); delay(100); }
    for(int i=0; i<4; i++) updateLEDs(i, counter[i]);
    displayLCD();
    startBell(maxDur, "Jadwal: " + labels);
  } else {
    // Reset jika sebelumnya aktif dari IoT
    bool needReset = false;
    for(int i=0; i<4; i++) {
      if(isIotActive[i]) { 
        counter[i] = 0; isIotActive[i] = false; updateLEDs(i, 0); needReset = true; 
      }
    }
    if(needReset) {
      Serial.println("[SCHEDULE] Reset status bel otomatis.");
      displayLCD();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n\n========================================");
  Serial.println("       SYSTEM STARTING (BEL VISUAL)     ");
  Serial.println("========================================");

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("[RTC] ERROR: DS3231 Tidak Ditemukan!");
  } else {
    Serial.println("[RTC] OK: DS3231 Terdeteksi.");
  }

  lcd.init();
  lcd.backlight();
  lcd.print("BOOTING...");

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] ERROR: LittleFS gagal.");
  } else {
    Serial.println("[FS] OK: LittleFS aktif.");
  }

  initHardware();

  LoRa.setPins(NSS, RST, DI0);
  if (LoRa.begin(433E6)) {
    LoRa.setSyncWord(0xF1);
    Serial.println("[LoRa] OK: Inisialisasi 433MHz Berhasil.");
  } else {
    Serial.println("[LoRa] ERROR: Inisialisasi Gagal.");
  }

  // Setup Buttons & LEDs...
  int btns[] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4, SUBMIT_BUTTON};
  for(int b : btns) pinMode(b, INPUT_PULLUP);
  int leds[] = {LED_GRN_1, LED_YLW_1, LED_RED_1, LED_GRN_2, LED_YLW_2, LED_RED_2, 
                LED_GRN_3, LED_YLW_3, LED_RED_3, LED_GRN_4, LED_YLW_4, LED_RED_4};
  for(int l : leds) { pinMode(l, OUTPUT); digitalWrite(l, LOW); }

  connectWiFi();
  syncTime();

  if (WiFi.status() == WL_CONNECTED) {
    sendStatus(); 
    xTaskCreatePinnedToCore(taskManualOverride, "ManualTask", 8192, NULL, 1, NULL, 1);
    Serial.println("[TASK] Manual Override Task dimulai pada Core 1.");
  }

  lcd.clear();
  displayLCD();
  Serial.println("[SYSTEM] Ready. Menjalankan Loop Utama...\n");
}

void loop() {
  handleBellStop(); 
  checkAndRun();

  // Polling Status Firebase (60 detik)
  if (millis() - lastStatusUpdate > 60000) {
    lastStatusUpdate = millis();
    if (WiFi.status() == WL_CONNECTED) sendStatus();
  }

  // Tombol Manual Jenjang
  static unsigned long lastBtnCheck = 0;
  if (millis() - lastBtnCheck > 50) {
    lastBtnCheck = millis();
    int btnPins[] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4};
    for (int i = 0; i < 4; i++) {
      if (digitalRead(btnPins[i]) == LOW) {
        counter[i]++;
        Serial.printf("[BUTTON] Jenjang %s di-klik. Mode: %d\n", JENJANG_NAMES[i], counter[i] % 5);
        updateLEDs(i, counter[i]);
        displayLCD();
        delay(250); 
      }
    }
  }

  // Tombol SUBMIT (Master Manual)
  if (digitalRead(SUBMIT_BUTTON) == LOW) {
    Serial.println("[BUTTON] SUBMIT (Master) Ditekan!");
    for(int i=0; i<3; i++) { sendLoRa(counter[0], counter[1], counter[2], counter[3]); delay(100); }
    startBell(5, "Manual Master");
    for(int i=0; i<4; i++) { counter[i] = 0; updateLEDs(i, 0); }
    lcd.clear();
    displayLCD();
  }

  // Cek Versi Jadwal Firebase (30 detik)
  if (WiFi.status() == WL_CONNECTED && millis() - lastFirebaseCheck > 30000) {
    lastFirebaseCheck = millis();
    String verStr;
    if (httpGET(FIREBASE_CONFIG, verStr)) {
      int cloudVer = verStr.toInt();
      int localVer = readFile("version").toInt();
      if (cloudVer != localVer) {
        Serial.printf("[SYNC] Perubahan Versi: Cloud(%d) vs Lokal(%d)\n", cloudVer, localVer);
        downloadAllDays(cloudVer);
      }
    }
  }
}
