# thrust-gauge-wifi — project context for Claude

## Hardware
- ESP32 WROOM
- Load cell: Dayang DYLY-102-20kg (S-type) → HX711 (24-bit ADC)
- Hall sensor: GPIO 4, INPUT_PULLUP, CHANGE interrupt, 2 pulses/rev

## Wiring
| From | To |
|------|----|
| HX711 DT | GPIO 21 |
| HX711 SCK | GPIO 22 |
| HX711 VCC | 5V |
| Hall sensor OUT | GPIO 4 |
| Hall sensor VCC | 3.3V |
| Tare button | GPIO 0 (BOOT button, active LOW) |

## Build & upload commands

```bash
# First time / USB fallback
pio run -t upload -e usb
pio run -t uploadfs -e usb      # uploads data/index.html to LittleFS

# Wireless (OTA) — ESP32 must be on WiFi
pio run -t upload -e ota
pio run -t uploadfs -e ota      # OTA HTML update, no cable needed

# Both environments build the same firmware — only upload method differs
```

OTA password: `thrust`

## WiFi credentials
In `src/main.cpp` lines 10–11:
```cpp
static const char* WIFI_SSID = "Kringel";
static const char* WIFI_PASS = "leipomo25";
```

## Access
- Dashboard: `http://thrust-gauge.local`
- WebSocket: `ws://thrust-gauge.local:81`
- IP (fallback): check router or Serial monitor at 115200 baud

## WebSocket API
ESP32 broadcasts JSON every HX711 reading (~2Hz):
```json
{"g": 1234.5, "avg": 1198.3, "rpm": 4250}
```
- `g` — current thrust in grams (can be negative)
- `avg` — 3-second sliding average in grams
- `rpm` — current RPM (0 if no pulse for 2s)

Browser → ESP32 commands (text):
- `tare` — zero the load cell + clear avg buffer
- `resetmax` — clear avg buffer only

## Serial commands (115200 baud, USB)
```
tare        — zero load cell
cal:<g>     — calibrate with known mass in grams (e.g. cal:758)
raw         — print raw HX711 value (for diagnostics)
```

## Calibration workflow
1. Disconnect weight → `tare`
2. Hang known mass (grams) → `cal:758`
3. Copy printed factor into `CALIBRATION_FACTOR` in `src/main.cpp`
4. `pio run -t upload -e ota`

Current factor: `-218.3272` (measured 2026-06-29 with Dayang DYLY-102)

## File structure
```
src/main.cpp        — ESP32 firmware
data/index.html     — dashboard (served from LittleFS via HTTP)
web/index.html      — same file, local copy for development
platformio.ini      — two envs: [usb] and [ota]
```

`data/` is what gets flashed to ESP32. `web/` is a working copy —
keep them in sync: after editing `web/index.html`, copy to `data/` before `uploadfs`.

## Key constants (src/main.cpp)
| Constant | Value | Notes |
|----------|-------|-------|
| CALIBRATION_FACTOR | -218.3272 | Recalibrate if load cell replaced |
| PULSES_PER_REV | 2 | 2 magnets on motor shaft |
| RPM_TIMEOUT_US | 2000000 | 2s no pulse → RPM=0 |
| AVG_WINDOW_MS | 3000 | 3s sliding average window |
| AVG_SAMPLES | 5 | HX711 averaging per reading |

## Current status
- [x] HX711 + load cell working, calibrated
- [x] WiFi + mDNS (thrust-gauge.local)
- [x] WebSocket dashboard with graph, Newton/gram/RPM display
- [x] OTA firmware + filesystem updates
- [x] Hall sensor RPM — code ready, sensor not yet wired
- [ ] Hall sensor physically connected (waiting for hardware)
