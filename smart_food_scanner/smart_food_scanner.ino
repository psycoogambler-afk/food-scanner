// ============================================================================
//  SMART FOOD FRESHNESS SCANNER - ESP32 firmware
//  ESP32 + AI Food Monitoring System
//
//  Flow:  Food Sample -> Sensors -> ESP32 -> Web Dashboard -> AI -> Result
//
//  Board : ESP32 DevKit (ESP32-WROOM-32)
//  Libs  : Adafruit SSD1306, Adafruit GFX, DHT sensor library,
//          WebSockets (Markus Sattler), Adafruit TCS34725 (optional)
//  Upload: flash this sketch, then upload the web/ folder to SPIFFS
//          (Arduino IDE: Tools -> ESP32 Sketch Data Upload)
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

// ---------- USER CONFIG -----------------------------------------------------
const char* WIFI_SSID     = "LearningLinksFoundation";
const char* WIFI_PASSWORD = "098765432";

const char* AI_SERVER_URL = "https://foodscannercs.onrender.com/analyze"; // Replace with your actual Render URL

// ---------- PINS ------------------------------------------------------------
#define PIN_MQ135      34     // ADC1_CH6  (analog gas sensor)
#define PIN_DHT         4
#define PIN_BTN        15     // to GND, uses internal pull-up
#define PIN_BUZZER     14     // active buzzer
#define PIN_LED_R      25
#define PIN_LED_G      26
#define PIN_LED_B      27
#define I2C_SDA        21
#define I2C_SCL        22
#define DHTTYPE        DHT22

// ---------- TUNABLES --------------------------------------------------------
#define SCAN_SETTLE_MS      10000   // chamber settle time per scan
#define TELEMETRY_MS         2000   // websocket broadcast interval
#define MQ_CALIBRATION_MS   20000   // warm-up / R0 calibration at boot
#define MQ_CLEANAIR_PPM    400.0    // assumed PPM of clean air

// ---------- OBJECTS ---------------------------------------------------------
DHT dht(PIN_DHT, DHTTYPE);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WebServer   server(80);
WebSocketsServer ws(81);

// ---------- STATE -----------------------------------------------------------
struct SensorState {
  float tempC   = 25.0;
  float humidity = 50.0;
  float gasPPM  = MQ_CLEANAIR_PPM;
  float r0      = 10.0;
  bool  dhtOk   = false;
  bool  colorOk = false;
  uint16_t colorR = 0, colorG = 0, colorB = 0;
} st;

enum SysState { IDLE, SCANNING, RESULT };
SysState sysState = IDLE;

String lastVerdict = "READY";
int    lastScore   = 100;
unsigned long lastTelemetry = 0;
unsigned long scanStart     = 0;

// ---------- HEURISTIC FRESHNESS ENGINE (mirrors ai/ai_analysis.js) ----------
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int computeScore(float temp, float hum, float ppm) {
  float score = 100.0;
  // Gas/VOC penalty (dominant spoilage signal): baseline 400 -> 2500+ PPM
  float gasRatio = (ppm - MQ_CLEANAIR_PPM) / (2500.0 - MQ_CLEANAIR_PPM);
  score -= clampf(gasRatio, 0.0, 1.0) * 45.0;
  // Temperature penalty: ideal 2-10C storage
  if (temp < 2)            score -= (2 - temp) * 2.0;
  else if (temp > 10)      score -= (temp - 10) * 2.0;
  score = clampf(score, 0, 100);
  // Humidity penalty: ideal 40-70 %
  if (hum < 40)            score -= (40 - hum) * 0.6;
  else if (hum > 70)       score -= (hum - 70) * 0.6;
  score = clampf(score, 0, 100);
  return (int)(score + 0.5);
}

const char* verdictOf(int s) {
  if (s >= 75) return "FRESH";
  if (s >= 45) return "CHECK FOOD";
  return "POSSIBLE SPOILAGE";
}

// ---------- HARDWARE HELPERS ------------------------------------------------
void setRGB(int r, int g, int b) {
  analogWrite(PIN_LED_R, r); analogWrite(PIN_LED_G, g); analogWrite(PIN_LED_B, b);
}
void beep(int ms) { digitalWrite(PIN_BUZZER, HIGH); delay(ms); digitalWrite(PIN_BUZZER, LOW); }

void showOLED(const char* line1, const char* line2, const char* line3) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);  display.println(line1);
  display.setCursor(0, 20); display.println(line2);
  display.setCursor(0, 40); display.println(line3);
  display.display();
}

float mqReadPPM() {
  // Rs from voltage divider; R0 calibrated at boot in clean air.
  int raw = analogRead(PIN_MQ135);                 // 0..4095
  float v = (raw / 4095.0) * 3.3;
  if (v < 0.01) v = 0.01;
  float rs = (3.3 - v) / v;                        // RL = 1k, ratio form
  float ratio = rs / st.r0;
  // log-fit approximation of the MQ-135 CO2/VOC curve
  float ppm = 116.602 * pow(ratio, -2.769) * 10.0;
  return ppm;
}

void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) { st.humidity = h; st.tempC = t; st.dhtOk = true; }
  st.gasPPM = mqReadPPM();
}

void calibrateMQ() {
  showOLED("MQ-135 warming up...", "Calibrating in", "clean air...");
  float acc = 0; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < MQ_CALIBRATION_MS) {
    int raw = analogRead(PIN_MQ135);
    float v = (raw / 4095.0) * 3.3; if (v < 0.01) v = 0.01;
    acc += (3.3 - v) / v; n++;
    delay(500);
  }
  st.r0 = acc / n;
  if (st.r0 < 0.1) st.r0 = 0.1;
  Serial.printf("[MQ135] R0 = %.3f\n", st.r0);
}

void ledForVerdict(const char* v) {
  if (strcmp(v, "FRESH") == 0)               setRGB(0, 255, 0);
  else if (strcmp(v, "CHECK FOOD") == 0)     setRGB(255, 160, 0);
  else                                       setRGB(255, 0, 0);
}

// ---------- SCAN SEQUENCE ---------------------------------------------------
void startScan() {
  if (sysState == SCANNING) return;
  sysState = SCANNING;
  scanStart = millis();
  setRGB(0, 0, 255);
  beep(80);
  Serial.println("[SCAN] Scanning Food...");
  showOLED("Scanning Food...", "Settling chamber", "10s ...");
}

void finishScan() {
  // average a few final readings for stability
  float t = 0, h = 0, g = 0;
  for (int i = 0; i < 10; i++) {
    readSensors();
    t += st.tempC; h += st.humidity; g += st.gasPPM;
    delay(100);
  }
  t /= 10; h /= 10; g /= 10;
  int score = computeScore(t, h, g);
  lastScore = score;
  lastVerdict = verdictOf(score);
  sysState = RESULT;

  char buf[200];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"scan_result\",\"temp\":%.1f,\"hum\":%.1f,\"gas\":%.0f,"
    "\"score\":%d,\"verdict\":\"%s\"}",
    t, h, g, score, lastVerdict.c_str());
  ws.broadcastTXT(buf);

  // Send to AI Server
  if (strlen(AI_SERVER_URL) > 0 && strncmp(AI_SERVER_URL, "http", 4) == 0) {
    WiFiClientSecure *client = new WiFiClientSecure;
    if(client) {
      client->setInsecure(); // ignore SSL certificate validation for simplicity
      HTTPClient http;
      if (http.begin(*client, AI_SERVER_URL)) {
        http.addHeader("Content-Type", "application/json");
        char payload[150];
        snprintf(payload, sizeof(payload), "{\"temp\":%.1f,\"hum\":%.1f,\"gas\":%.0f}", t, h, g);
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
          Serial.printf("[AI Server] POST... code: %d\n", httpCode);
          String response = http.getString();
          Serial.println(response);
        } else {
          Serial.printf("[AI Server] POST failed, error: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();
      }
      delete client;
    }
  }

  ledForVerdict(lastVerdict.c_str());
  beep(120);
  Serial.printf("[SCAN] score=%d verdict=%s\n", score, lastVerdict.c_str());
}

// ---------- WEBSOCKET -------------------------------------------------------
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("[WS] client #%u connected\n", num);
  } else if (type == WStype_TEXT) {
    String msg = String((char*)payload);
    if (msg.indexOf("\"cmd\":\"scan\"") >= 0 || msg.indexOf("scan") >= 0) {
      startScan();
    }
  }
}

// ---------- HTTP ------------------------------------------------------------
void handleRoot() {
  File f = SPIFFS.open("/dashboard.html", "r");
  if (!f) { server.send(500, "text/plain", "dashboard.html missing - upload web/ to SPIFFS"); return; }
  server.streamFile(f, "text/html");
  f.close();
}

void handleStatus() {
  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"hum\":%.1f,\"gas\":%.0f,\"score\":%d,"
    "\"verdict\":\"%s\",\"state\":\"%s\",\"rssi\":%d}",
    st.tempC, st.humidity, st.gasPPM, lastScore, lastVerdict.c_str(),
    sysState == SCANNING ? "SCANNING" : (sysState == RESULT ? "RESULT" : "IDLE"),
    WiFi.RSSI());
  server.send(200, "application/json", buf);
}

// ---------- SETUP ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_MQ135, INPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED init failed - check wiring");
  }
  dht.begin();

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

  showOLED("SMART FOOD", "FRESHNESS", "SCANNER v1.0");
  calibrateMQ();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin("foodscanner")) {
    Serial.println("mDNS responder started. You can now access the dashboard at: http://foodscanner.local");
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.begin();
  ws.begin();
  ws.onEvent(wsEvent);

  setRGB(0, 255, 0);
  showOLED("ESP32 READY", WiFi.localIP().toString().c_str(), "Press button / web");
}

// ---------- LOOP -------------------------------------------------------------
void loop() {
  server.handleClient();
  ws.loop();
  readSensors();

  // physical button starts a scan too
  if (digitalRead(PIN_BTN) == LOW) { delay(200); startScan(); }

  // chamber settle countdown while scanning
  if (sysState == SCANNING) {
    unsigned long elapsed = millis() - scanStart;
    if (elapsed >= SCAN_SETTLE_MS) finishScan();
    else {
      char l2[24]; snprintf(l2, sizeof(l2), "Settling: %lus", (SCAN_SETTLE_MS - elapsed) / 1000);
      showOLED("Scanning Food...", l2, "Keep chamber shut");
    }
  } else if (millis() - lastTelemetry > TELEMETRY_MS) {
    char l2[24], l3[24];
    snprintf(l2, sizeof(l2), "T:%.1fC H:%.0f%%", st.tempC, st.humidity);
    snprintf(l3, sizeof(l3), "Gas:%.0fppm", st.gasPPM);
    showOLED(lastVerdict.c_str(), l2, l3);
  }

  // live telemetry broadcast
  if (millis() - lastTelemetry > TELEMETRY_MS) {
    lastTelemetry = millis();
    char buf[200];
    snprintf(buf, sizeof(buf),
      "{\"type\":\"telemetry\",\"temp\":%.1f,\"hum\":%.1f,\"gas\":%.0f,"
      "\"score\":%d,\"verdict\":\"%s\",\"state\":\"%s\"}",
      st.tempC, st.humidity, st.gasPPM, lastScore, lastVerdict.c_str(),
      sysState == SCANNING ? "SCANNING" : "IDLE");
    ws.broadcastTXT(buf);
  }
}
