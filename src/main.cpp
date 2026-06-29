#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
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
static constexpr uint8_t  AVG_SAMPLES        = 5;
static constexpr uint32_t AVG_WINDOW_MS      = 3000;
static constexpr float    CALIBRATION_FACTOR = -218.3272f;
static constexpr uint8_t  PULSES_PER_REV     = 2;
static constexpr uint32_t RPM_TIMEOUT_US     = 2000000UL;

// ── Hall sensor RPM ───────────────────────────────────────────────────────────
static volatile uint32_t hallLastUs  = 0;
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

// ── Rolling max ───────────────────────────────────────────────────────────────
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
    for (uint8_t i = 0; i < bufCount; i++) {
        if (buf[i].ts >= cutoff) { sum += buf[i].g; n++; }
    }
    return n ? sum / n : 0.0f;
}

static void clearMax() { bufCount = 0; bufHead = 0; }

// ── HX711 ─────────────────────────────────────────────────────────────────────
HX711 scale;

// ── WebSocket ─────────────────────────────────────────────────────────────────
WebServer        http(80);
WebSocketsServer ws(81);

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t) {
    if (type != WStype_TEXT) return;
    String msg = String((char*)payload);
    msg.trim();
    if (msg == "tare") {
        scale.tare();
        clearMax();
        Serial.println("Tare (WebSocket)");
    } else if (msg == "resetmax") {
        clearMax();
        Serial.println("Max reset (WebSocket)");
    }
}

// ── Serial calibration ────────────────────────────────────────────────────────
static void handleSerial() {
    if (!Serial.available()) return;
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s == "tare") {
        scale.tare();
        clearMax();
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
    if (digitalRead(PIN_TARE) == LOW) {
        if (!pressed) {
            pressed = true;
            scale.tare();
            clearMax();
            Serial.println("Tare OK");
        }
    } else {
        pressed = false;
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
    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("thrust-gauge"))
        Serial.println("mDNS: thrust-gauge.local");

    ArduinoOTA.setHostname("thrust-gauge");
    ArduinoOTA.setPassword("thrust");
    ArduinoOTA.begin();
    Serial.println("OTA ready (password: thrust)");

    LittleFS.begin(true);
    http.on("/", HTTP_GET, []() {
        File f = LittleFS.open("/index.html", "r");
        if (f) { http.streamFile(f, "text/html"); f.close(); }
        else     http.send(404, "text/plain", "index.html not found");
    });
    http.begin();
    Serial.println("HTTP: http://thrust-gauge.local");

    ws.begin();
    ws.onEvent(onWsEvent);
    Serial.println("WebSocket: ws://thrust-gauge.local:81");
}

void loop() {
    ArduinoOTA.handle();
    http.handleClient();
    ws.loop();
    handleSerial();
    handleTare();

    if (!scale.is_ready()) return;

    float thrust = scale.get_units(AVG_SAMPLES);

    pushReading(thrust);
    float avgG = slidingAvg();
    float rpm  = calcRpm();

    String msg = "{\"g\":" + String(thrust, 1) +
                 ",\"avg\":" + String(avgG, 1) +
                 ",\"rpm\":" + String(rpm, 0) + "}";
    ws.broadcastTXT(msg);

    Serial.printf("%.1f g  avg(3s)=%.1f g  rpm=%.0f\n", thrust, avgG, rpm);
}
