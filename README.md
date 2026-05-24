# The Pencreus — ESP32-S3-RLCD-4.2 Weather Station

Retro terminal aesthetic on a sunlight-readable 4.2" reflective LCD.
Boot splash with brand character, live clock (NTP + RTC), outdoor weather with sunrise/sunset,
indoor temp/humidity, and an animated Eyebot orb mascot with listening/responding states.

## Hardware
| Part | Notes |
|------|-------|
| Waveshare ESP32-S3-RLCD-4.2 | 400×300 reflective LCD, no backlight needed |
| Onboard SHTC3 | Indoor temp + humidity |
| Onboard PCF85063 RTC | Keeps time without WiFi |
| 18650 battery holder | Very long runtime — no backlight drain |

## Wiring
Everything is onboard — no external connections needed.

## Arduino IDE Setup
1. Add ESP32 board manager URL:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Install **esp32 by Espressif** 2.0.x
3. Board settings: **ESP32S3 Dev Module**
   - Flash Size: 16MB
   - PSRAM: OPI PSRAM 8MB
   - Upload Speed: 921600
4. Install via Library Manager:
   - **U8g2** by Oliver Kraus
   - **ArduinoJson** by Benoît Blanchon
5. Install Waveshare custom libraries (only SensorLib + U8g2 needed — skip LVGL):
   ```bash
   git clone https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
   cp -r ESP32-S3-RLCD-4.2/01_Arduino_Libraries/SensorLib ~/Arduino/libraries/
   cp -r ESP32-S3-RLCD-4.2/01_Arduino_Libraries/U8g2     ~/Arduino/libraries/
   ```

## Configuration
Edit `ESP32_RLCD_Display/secrets.h` before flashing:
- `WIFI_SSID` / `WIFI_PASS` — your network (pre-filled)
- `OWM_API_KEY` — OpenWeatherMap key (pre-filled)
- `OWM_LAT` / `OWM_LON` — your GPS coordinates
- `HA_TOKEN` — Home Assistant long-lived token (optional)

## File Structure
```
ESP32-RLCD/
├── mockup/
│   ├── index.html          ← Live browser mockup (http://localhost:7842)
│   ├── logo.svg            ← Brand character (splash only)
│   ├── logo_1bit.png       ← 56×72 px 1-bit reference
│   └── logo_splash.png     ← 120×154 px splash reference
├── ESP32_RLCD_Display/
│   ├── ESP32_RLCD_Display.ino  ← Main sketch
│   ├── config.h                ← Pins, addresses, intervals
│   ├── secrets.h               ← WiFi + API keys (git-ignored)
│   ├── splash_logo_xbm.h       ← 120×154 px boot splash bitmap
│   └── logo_xbm.h              ← 56×72 px (unused — kept for reference)
├── .gitignore
└── README.md
```

## Display Layout (400×300 landscape)
```
┌─ THE PENCREUS  //  PERSONAL STATUS MONITOR ────────────────┐  ← header
│  THU              │  21:59:22                               │  ← date/clock
│  21 MAY 2026      │              LOCAL · PST8PDT            │
├───────────────────────────────────────────────────────────┤
│  // OUTDOOR WEATHER [OWM]                                  │
│  Partly Cloudy    72°F     HUMIDITY 45%  FEELS 69°F        │
│  H:78° · L:61°   SR:06:14 · SS:20:47                      │
├───────────────────────────────────────────────────────────┤
│  // INTERIOR [SHTC3]          │  [EYEBOT ORB ANIMATION]   │
│  TEMP  68.4°F                 │    ◎  floating / pulsing  │
│  RH    45.1%                  │                            │
├───────────────────────────────────────────────────────────┤
│  ● WIFI:OK  |  SSID:AROUTERSPACE  |  STATUS:NOMINAL        │
└───────────────────────────────────────────────────────────┘
```

## KEY Button
Press the onboard KEY button (GPIO 0) to cycle:
**IDLE** → **LISTENING** (pulse rings + antenna wiggle) → **RESPONDING** (speech arcs) → **IDLE**
