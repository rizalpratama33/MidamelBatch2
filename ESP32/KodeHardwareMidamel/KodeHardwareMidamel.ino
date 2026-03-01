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

/* ================= WIFI ================= */
#define WIFI_SSID "Kadus puhun"
#define WIFI_PASSWORD "1234567899"

/* ================= FIREBASE ================= */
#define FIREBASE_HOST "https://bell-visual-midamel-default-rtdb.firebaseio.com/devices/bel_sekolah_1"
#define FIREBASE_JADWAL FIREBASE_HOST "/jadwal"
#define FIREBASE_CONFIG FIREBASE_HOST "/config/version.json"
#define FIREBASE_STATUS FIREBASE_HOST "/status.json"
#define FIREBASE_OVERRIDE FIREBASE_HOST "/manual_override.json"

/* ================= PIN LORA ================= */
#define NSS 5
#define RST 14
#define DI0 10

/* ================= PIN PANEL KONTROL (MASTER) ================= */
#define SUBMIT_LED 6
#define SUBMIT_BUTTON 18
#define BELL_PIN 25 // Relay Master
#define LED_PIN 2   // LED Status Master

// TK (A)
#define BUTTON_1 19
#define LED_GRN_1 48
#define LED_YLW_1 47
#define LED_RED_1 20

// SD (B)
#define BUTTON_2 35 
#define LED_GRN_2 38
#define LED_YLW_2 37
#define LED_RED_2 36

// SMP (C)
#define BUTTON_3 39 
#define LED_GRN_3 42
#define LED_YLW_3 41
#define LED_RED_3 40

// SMA (D)
#define BUTTON_4 3 
#define LED_GRN_4 15
#define LED_YLW_4 16
#define LED_RED_4 17

/* ================= PIN LCD I2C ================= */
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
int serverVersion = -1;

/* ================= BELL TIMER & MUTEX ================= */
hw_timer_t *bellTimer = NULL;
volatile bool bellStopFlag = false;
volatile bool bellActive = false;
SemaphoreHandle_t bellMutex; 

void IRAM_ATTR onBellTimer() {
  bellStopFlag = true;
}

void initBell() {
  pinMode(BELL_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SUBMIT_LED, OUTPUT);
  digitalWrite(BELL_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(SUBMIT_LED, LOW);

  bellTimer = timerBegin(0, 80, true); 
  timerAttachInterrupt(bellTimer, &onBellTimer, true);
  
  bellMutex = xSemaphoreCreateMutex();
  Serial.println("[SISTEM] Perkakasan Loceng/Lampu Sedia");
}

void startBell(uint16_t dur, String label) {
  if (xSemaphoreTake(bellMutex, 0) == pdTRUE) {
    if (bellActive) {
      xSemaphoreGive(bellMutex);
      return;
    }
    bellActive = true;
    xSemaphoreGive(bellMutex);

    digitalWrite(BELL_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH); 
    digitalWrite(SUBMIT_LED, HIGH);

    timerAlarmWrite(bellTimer, (uint64_t)dur * 1000000ULL, false);
    timerAlarmEnable(bellTimer);

    Serial.printf("\n>>> [AKTIF] %s (%d saat)\n", label.c_str(), dur);
  }
}

void handleBellStop() {
  if (!bellStopFlag) return;
  bellStopFlag = false;
  
  digitalWrite(BELL_PIN, LOW);
  digitalWrite(LED_PIN, LOW); 
  digitalWrite(SUBMIT_LED, LOW);
  timerAlarmDisable(bellTimer); 
  
  xSemaphoreTake(bellMutex, portMAX_DELAY);
  bellActive = false;
  xSemaphoreGive(bellMutex);
  
  Serial.println(">>> [MATI] Loceng/Lampu Berhenti");
}

/* ================= FUNGSI LORA & UI ================= */
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
  for (int i = 0; i < 4; i++) {
    lcd.setCursor(0, i);
    String label = String(JENJANG_NAMES[i]);
    label.toUpperCase();
    lcd.print(label + ": " + getModeName(counter[i]));
  }
}

void sendLoRa(int a, int b, int c, int d) {
  digitalWrite(SUBMIT_LED, HIGH);
  String packet = String(a % 5) + ";" + String(b % 5) + ";" + String(c % 5) + ";" + String(d % 5) + ";";
  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();
  Serial.println("[LoRa] Terkirim: " + packet);
  digitalWrite(SUBMIT_LED, LOW);
}

/* ================= FUNGSI FAIL LittleFS ================= */
String readFile(String filename) {
  File file = LittleFS.open("/" + filename + ".json", "r");
  if (!file) return "";
  String content = file.readString();
  file.close();
  return content;
}

bool writeFile(String filename, String content) {
  File file = LittleFS.open("/" + filename + ".json", "w");
  if (!file) return false;
  file.print(content);
  file.close();
  return true;
}

/* ================= HTTP GET/PUT ================= */
bool httpGET(String url, String &out) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); 
  http.begin(client, url);
  int code = http.GET();
  if (code == 200) out = http.getString();
  http.end();
  return code == 200;
}

bool httpPUT(String url, String payload) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, url);
  int code = http.PUT(payload);
  http.end();
  return code == 200;
}

/* ================= FUNGSI WIFI (RELIABLE) ================= */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  Serial.printf("\n[WIFI] Menghubungkan ke %s...\n", WIFI_SSID);
  lcd.clear();
  lcd.print("WiFi Connecting...");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);

  WiFi.disconnect(); 
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 30000) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(0, 2);
    lcd.print("Wait: ");
    lcd.print((millis() - startAttempt) / 1000);
    lcd.print("s");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Terhubung!");
    lcd.clear();
    lcd.print("WiFi Connected!");
    delay(1000);
  } else {
    Serial.printf("\n[WIFI] Gagal! Status: %d\n", WiFi.status());
    lcd.clear();
    lcd.print("WiFi Failed!");
    delay(2000);
  }
}

/* ================= STATUS KE FIREBASE ================= */
void sendStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  JsonDocument doc;
  doc["is_online"] = true;
  
  // Ambil waktu Unix dan konversi ke milidetik
  time_t now;
  time(&now);
  doc["last_seen"] = (long long)now * 1000;
  
  doc["wifi"] = WiFi.RSSI();
  
  String payload;
  serializeJson(doc, payload);
  httpPUT(FIREBASE_STATUS, payload);
  Serial.println("[STATUS] Diperbarui di Cloud");
}

/* ================= TASK MANUAL OVERRIDE (CORE 0) ================= */
void taskManualOverride(void *pvParameters) {
  Serial.println("[TASK] Manual Override Berjalan di Core 0");
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String payload;
      if (httpGET(FIREBASE_OVERRIDE, payload) && payload != "null") {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error && doc["trigger"] == true) {
          String target = doc["target"] | "Manual";
          
          // Trigger Bel Lokal
          startBell(10, "Manual Override: " + target);

          // Kirim sinyal LoRa Darurat ke Slave (Mode 4)
          for(int k=0; k<5; k++) {
             sendLoRa(4, 4, 4, 4);
             delay(100);
          }

          // Reset trigger di Firebase
          doc["trigger"] = false;
          String response;
          serializeJson(doc, response);
          httpPUT(FIREBASE_OVERRIDE, response);
          Serial.println("[OVERRIDE] Selesai.");
        }
      }
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

/* ================= LOGIK IOT & SYNC ================= */
void downloadAllDays(int newVer) {
  lcd.clear();
  lcd.print("Updating Cloud...");
  for (int i = 0; i < 7; i++) {
    String payload;
    if (httpGET(String(FIREBASE_JADWAL) + "/" + HARI[i] + ".json", payload) && payload != "null") {
      writeFile(HARI[i], payload);
    }
  }
  File vFile = LittleFS.open("/version.txt", "w");
  if(vFile) { vFile.print(String(newVer)); vFile.close(); }
  lcd.clear();
  displayLCD();
}

void checkAndRun() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  int currentTotalMin = t.tm_hour * 60 + t.tm_min;
  if (currentTotalMin == lastMinuteTrigger) return;

  String hari = getHariSekarang();
  String json = readFile(hari);
  if (json.length() < 10) return;

  JsonDocument doc;
  deserializeJson(doc, json);

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
      if (data.is<JsonArray>()) for (JsonObject it : data.as<JsonArray>()) process(it);
      else if (data.is<JsonObject>()) for (JsonPair kv : data.as<JsonObject>()) process(kv.value().as<JsonObject>());
    }
  }

  if (anyTriggered) {
    for(int k=0; k<5; k++) { 
       sendLoRa(counter[0], counter[1], counter[2], counter[3]);
       delay(200);
    }
    for(int i=0; i<4; i++) updateLEDs(i, counter[i]);
    displayLCD();
    startBell(maxDur, "Jadwal: " + labels);
  } else {
    for(int i=0; i<4; i++) {
      if(isIotActive[i]) {
        counter[i] = 0;
        isIotActive[i] = false;
        updateLEDs(i, 0);
      }
    }
    displayLCD();
  }
}

String getHariSekarang() {
  struct tm t;
  if (!getLocalTime(&t)) return "senin";
  int w = t.tm_wday; 
  if (w == 0) w = 7; 
  return String(HARI[w - 1]);
}

/* ================= MAIN SETUP & LOOP ================= */
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.print("MASTER CONTROLLER");
  
  if (!LittleFS.begin(true)) Serial.println("FS Error");

  initBell();

  // LoRa Setup
  LoRa.setPins(NSS, RST, DI0);
  if (!LoRa.begin(433E6)) Serial.println("LoRa Error");
  LoRa.setSyncWord(0xF1);

  // Pin Config
  int btns[] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4, SUBMIT_BUTTON};
  for(int b : btns) pinMode(b, INPUT_PULLUP);
  
  int leds[] = {LED_GRN_1, LED_YLW_1, LED_RED_1, LED_GRN_2, LED_YLW_2, LED_RED_2, 
                LED_GRN_3, LED_YLW_3, LED_RED_3, LED_GRN_4, LED_YLW_4, LED_RED_4};
  for(int l : leds) { pinMode(l, OUTPUT); digitalWrite(l, LOW); }

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(7 * 3600, 0, "pool.ntp.org");
    delay(2000); 
    
    sendStatus(); 

    xTaskCreatePinnedToCore(taskManualOverride, "ManualTask", 8192, NULL, 3, NULL, 0);
  }

  lcd.clear();
  displayLCD();
}

void loop() {
  handleBellStop();
  checkAndRun();

  // Update Status Berkala (setiap 60 detik)
  if (millis() - lastStatusUpdate > 60000) {
    lastStatusUpdate = millis();
    sendStatus();
  }

  // WiFi Reconnection Logic
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 60000) { 
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
  }

  // Manual Buttons
  bool changed = false;
  int btnPins[] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4};
  for (int i = 0; i < 4; i++) {
    if (digitalRead(btnPins[i]) == LOW) {
      counter[i]++;
      updateLEDs(i, counter[i]);
      changed = true;
      delay(300);
    }
  }
  if (changed) displayLCD();

  // Manual Submit
  if (digitalRead(SUBMIT_BUTTON) == LOW) {
    lcd.setCursor(0,0); lcd.print("SENDING MANUAL...   ");
    for(int i=0; i<8; i++) {
       sendLoRa(counter[0], counter[1], counter[2], counter[3]);
       delay(150);
    }
    startBell(5, "Manual Master");
    for(int i=0; i<4; i++) { counter[i] = 0; updateLEDs(i, 0); }
    lcd.clear();
    displayLCD();
  }

  // IoT Sync berkala
  if (WiFi.status() == WL_CONNECTED && millis() - lastFirebaseCheck > 30000) {
    lastFirebaseCheck = millis();
    String verStr;
    if (httpGET(FIREBASE_CONFIG, verStr)) {
      int cloudVer = verStr.toInt();
      int localVer = -1;
      File vFile = LittleFS.open("/version.txt", "r");
      if(vFile) { localVer = vFile.readString().toInt(); vFile.close(); }
      if (cloudVer != localVer) downloadAllDays(cloudVer);
    }
  }
}