#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <HX711.h>

// ── WiFi credentials ──────────────────────────────────────────────────────────
static const char* WIFI_SSID = "Kringel";
static const char* WIFI_PASS = "leipomo25";

// ── Pins ──────────────────────────────────────────────────────────────────────
static constexpr uint8_t PIN_HX_DT  = 21;
static constexpr uint8_t PIN_HX_SCK = 22;
static constexpr uint8_t PIN_TARE   = 0;
static constexpr uint8_t PIN_HALL   = 4;

// ── Config ────────────────────────────────────────────────────────────────────
static constexpr uint8_t  AVG_SAMPLES        = 1;
static constexpr uint32_t AVG_WINDOW_MS      = 3000;
static constexpr float    CALIBRATION_FACTOR = -218.3272f;
static constexpr uint8_t  PULSES_PER_REV     = 2;
static constexpr uint32_t RPM_TIMEOUT_US     = 2000000UL;

// ── Hall sensor RPM ───────────────────────────────────────────────────────────
static volatile uint32_t hallLastUs   = 0;
static volatile uint32_t hallPeriodUs = 0;

void IRAM_ATTR onHallPulse() {
    uint32_t now = micros();
    uint32_t dt  = now - hallLastUs;
    if (dt < 1000) return;
    hallPeriodUs = dt;
    hallLastUs   = now;
}

static float calcRpm() {
    noInterrupts();
    uint32_t period = hallPeriodUs;
    uint32_t lastUs = hallLastUs;
    interrupts();
    if (period == 0 || (micros() - lastUs) > RPM_TIMEOUT_US) return 0.0f;
    return 60000000.0f / ((float)period * PULSES_PER_REV);
}

// ── Rolling average ───────────────────────────────────────────────────────────
static constexpr uint8_t BUF_LEN = 24;
struct Reading { uint32_t ts; float g; };
static Reading buf[BUF_LEN];
static uint8_t bufHead = 0, bufCount = 0;

static void pushReading(float g) {
    buf[bufHead] = { millis(), g };
    bufHead = (bufHead + 1) % BUF_LEN;
    if (bufCount < BUF_LEN) bufCount++;
}

static float slidingAvg() {
    uint32_t cutoff = millis() - AVG_WINDOW_MS;
    float sum = 0.0f;
    uint8_t n = 0;
    for (uint8_t i = 0; i < bufCount; i++)
        if (buf[i].ts >= cutoff) { sum += buf[i].g; n++; }
    return n ? sum / n : 0.0f;
}

static void clearBuf() { bufCount = 0; bufHead = 0; }

// ── HX711 ─────────────────────────────────────────────────────────────────────
HX711 scale;

// ── Async web server + WebSocket ──────────────────────────────────────────────
AsyncWebServer  http(80);
AsyncWebSocket  ws("/ws");

static volatile bool pendingTare = false;

static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type,
                      void* arg, uint8_t* data, size_t len) {
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) return;
    String msg = String((char*)data).substring(0, len);
    msg.trim();
    if (msg == "tare") pendingTare = true;
}

// ── Serial calibration ────────────────────────────────────────────────────────
static void handleSerial() {
    if (!Serial.available()) return;
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s == "tare") {
        scale.tare();
        clearBuf();
        Serial.println("Tare OK");
    } else if (s.startsWith("cal:")) {
        float knownG = s.substring(4).toFloat();
        if (knownG <= 0.0f) { Serial.println("ERROR: value must be > 0"); return; }
        float factor = scale.get_value(10) / knownG;
        scale.set_scale(factor);
        Serial.printf("Factor: %.4f\nUpdate: CALIBRATION_FACTOR = %.4ff\n", factor, factor);
    } else if (s == "raw") {
        Serial.printf("Raw: %.0f\n", scale.get_value(5));
    } else {
        Serial.println("Commands: tare | cal:<g> | raw");
    }
}

// ── Tare button (GPIO 0, active LOW) ─────────────────────────────────────────
static void handleTare() {
    static bool pressed = false;
    bool btnPressed = (digitalRead(PIN_TARE) == LOW);
    if (btnPressed && !pressed) pendingTare = true;
    pressed = btnPressed;
}

// ── WiFi reconnect watchdog ───────────────────────────────────────────────────
static void handleWifi() {
    static uint32_t lastCheck = 0;
    if (millis() - lastCheck < 5000) return;
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
}

// ── setup / loop ──────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    scale.begin(PIN_HX_DT, PIN_HX_SCK);
    scale.set_scale(CALIBRATION_FACTOR);
    scale.tare();
    pinMode(PIN_TARE, INPUT_PULLUP);
    pinMode(PIN_HALL, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_HALL), onHallPulse, CHANGE);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    WiFi.setSleep(false);
    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("thrust-gauge"))
        Serial.println("mDNS: thrust-gauge.local");

    ArduinoOTA.setHostname("thrust-gauge");
    ArduinoOTA.setPassword("thrust");
    ArduinoOTA.begin();
    Serial.println("OTA ready (password: thrust)");

    LittleFS.begin(true);
    ws.onEvent(onWsEvent);
    http.addHandler(&ws);
    http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    http.begin();
    Serial.println("HTTP: http://thrust-gauge.local  WS: ws://thrust-gauge.local/ws");
}

void loop() {
    handleWifi();
    ArduinoOTA.handle();
    ws.cleanupClients();
    handleSerial();
    handleTare();

    if (pendingTare) {
        pendingTare = false;
        scale.tare();
        clearBuf();
        Serial.println("Tare OK");
    }

    if (!scale.is_ready()) return;

    float thrust = scale.get_units(AVG_SAMPLES);
    pushReading(thrust);

    static uint32_t lastSend = 0;
    uint32_t now = millis();
    if (now - lastSend < 100) return;
    lastSend = now;

    float avgG = slidingAvg();
    float rpm  = calcRpm();

    char msg[64];
    snprintf(msg, sizeof(msg), "{\"g\":%.1f,\"avg\":%.1f,\"rpm\":%.0f}", thrust, avgG, rpm);
    ws.textAll(msg);

    Serial.printf("%.1f g  avg(3s)=%.1f g  rpm=%.0f\n", thrust, avgG, rpm);
}
