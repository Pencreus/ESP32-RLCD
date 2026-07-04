/*
 * PIP-BOY 3000 Mk.II — WEATHER STATION
 * Waveshare ESP32-S3-RLCD-4.2
 *
 * Display   : ST7305, 300×400 reflective LCD (no backlight needed)
 * Sensors   : SHTC3 (temp/humidity onboard), PCF85063 RTC
 * Network   : WiFi → NTP sync + OpenWeatherMap (temp, weather, sunrise, sunset)
 * UI        : Pip-Boy / Fallout terminal aesthetic via U8G2
 *             Eyebot orb mascot with IDLE / LISTENING / RESPONDING animations
 *
 * KEY button (GPIO 0) cycles:  IDLE → LISTENING → RESPONDING → IDLE
 *
 * Required libraries (Arduino Library Manager):
 *   • U8g2         by Oliver Kraus
 *   • ArduinoJson  by Benoît Blanchon
 *
 * Waveshare custom libraries (copy from their GitHub 01_Arduino_Libraries/):
 *   https://github.com/waveshareteam/ESP32-S3-RLCD-4.2
 *   → SensorLib  (SHTC3, RTC)
 *   → U8g2       (patched with ST7305 support)
 *
 * Board: ESP32S3 Dev Module | Flash: 16MB | PSRAM: OPI 8MB | Upload: 921600
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// Waveshare board support (local .h files — live in the sketch folder)
#include "ST7305_U8g2.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"

#include "config.h"
#include "secrets.h"
#include "splash_logo_xbm.h"   // 120×154 px character — boot splash only

// ── Hardware ──────────────────────────────────────────────────
static ST7305_U8g2 lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN,
                        RLCD_DC_PIN,  RLCD_CS_PIN, RLCD_RST_PIN);
I2cMasterBus  i2cBus(I2C_SDA_PIN, I2C_SCL_PIN, 0);
Shtc3Port*    shtc3 = nullptr;

// ── Display state ─────────────────────────────────────────────
enum DispState { STATE_IDLE, STATE_LISTENING, STATE_RESPONDING };
volatile DispState  dispState       = STATE_IDLE;
volatile bool       keyPressed      = false;
unsigned long       lastKeyMs       = 0;

// ── Data structs ──────────────────────────────────────────────
struct WeatherData {
  char  description[32] = "---";
  float tempF      = 0;
  float tempMinF   = 0;
  float tempMaxF   = 0;
  float feelsF     = 0;
  int   humidity   = 0;
  time_t sunrise   = 0;   // Unix timestamp (local after conversion)
  time_t sunset    = 0;
  bool  valid      = false;
};

struct SensorData {
  float tempF    = 0;
  float humidity = 0;
  bool  valid    = false;
};

WeatherData weather;
SensorData  indoor;

bool wifiConnected  = false;
bool ntpSynced      = false;
bool weatherFetched = false;

unsigned long lastWeatherMs  = 0;
unsigned long lastSensorMs   = 0;
unsigned long lastClockMs    = 0;
unsigned long lastBatteryMs  = 0;

uint8_t batteryLevel = 255;   // 255 = not yet read

// ── MQTT ──────────────────────────────────────────────────────
WiFiClient   wifiMqtt;
PubSubClient mqtt(wifiMqtt);
bool         mqttConnected  = false;
unsigned long lastMqttMs    = 0;   // throttle reconnect attempts

// ── KEY button ISR ────────────────────────────────────────────
void IRAM_ATTR onKeyPress() {
  keyPressed = true;
}

// ── Battery read — GPIO 4 ADC, 3:1 voltage divider ───────────
// Average 10 samples to reduce ADC noise.
void readBattery() {
  uint32_t sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
    delayMicroseconds(200);
  }
  float vol = (sum / 10.0f) * BAT_DIVIDER / 1000.0f;
  if (vol < BAT_V_MIN) {
    batteryLevel = 0;
  } else if (vol >= BAT_V_MAX) {
    batteryLevel = 100;
  } else {
    batteryLevel = (uint8_t)(((vol - BAT_V_MIN) / (BAT_V_MAX - BAT_V_MIN)) * 100.0f);
  }
}

// ── MQTT callback — fires on every incoming message ───────────
// Payload: "LISTENING" | "RESPONDING" | "IDLE"
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  if (len == 0 || len > 16) return;
  char msg[17] = {0};
  memcpy(msg, payload, len);

  if      (strcmp(msg, "LISTENING")  == 0) dispState = STATE_LISTENING;
  else if (strcmp(msg, "RESPONDING") == 0) dispState = STATE_RESPONDING;
  else if (strcmp(msg, "IDLE")       == 0) dispState = STATE_IDLE;

  Serial.printf("[MQTT] %s → %s\n", topic, msg);
}

// ── Connect / re-connect to MQTT broker ───────────────────────
void connectMQTT() {
  if (!wifiConnected) return;
  mqtt.setServer(MQTT_BROKER, MQTT_PORT_N);
  mqtt.setCallback(mqttCallback);

  // LWT: if the ESP32 drops off, broker publishes "offline" automatically
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                   MQTT_TOPIC_STATUS, 0, /*retain*/true, "offline")) {
    mqtt.subscribe(MQTT_TOPIC_STATE);
    mqtt.publish(MQTT_TOPIC_STATUS, "online", /*retain*/true);
    mqttConnected = true;
    Serial.println("[MQTT] connected, subscribed to " MQTT_TOPIC_STATE);
  } else {
    mqttConnected = false;
    Serial.printf("[MQTT] connect failed, rc=%d\n", mqtt.state());
  }
}

// ── Helpers ───────────────────────────────────────────────────
void titleCase(char* s) {
  if (s && *s >= 'a' && *s <= 'z') *s -= 32;
}

void fmtTime12(time_t ts, char* out, int sz) {
  struct tm t;
  localtime_r(&ts, &t);
  int h = t.tm_hour % 12;
  if (h == 0) h = 12;
  snprintf(out, sz, "%02d:%02d", h, t.tm_min);
}

// ── ORB ANIMATION ─────────────────────────────────────────────
// Draws the Pip-Boy Eyebot orb mascot using U8G2 primitives.
// cx/cy = center of orb body.  state drives animation variant.
//
// Orb radius 28 — scaled up ~1.33× from original r=21
void drawOrb(U8G2* u, int cx, int cy, DispState state) {
  uint32_t t = millis();

  // Floating bob: ±3 px over 2.4 s
  int bob = (int)(3.0f * sinf(t * 0.00262f));
  cy += bob;

  // ── Listening pulse rings ──────────────────────────────────
  if (state == STATE_LISTENING) {
    for (int i = 0; i < 3; i++) {
      uint32_t phase = (t + i * 460) % 1380;
      if (phase < 1100) {
        int r = 29 + phase / 37;
        u->drawCircle(cx, cy, r);
      }
    }
  }

  // ── Responding arcs (speech waves to the right) ───────────
  if (state == STATE_RESPONDING) {
    for (int i = 0; i < 3; i++) {
      uint32_t phase = (t + i * 333) % 1000;
      if (phase < 800) {
        int ox = phase / 30;
        int r  = 6 + phase / 50;
        u->drawCircle(cx + 26 + ox, cy, r);
      }
    }
  }

  // ── Glow ring during active states ────────────────────────
  if (state != STATE_IDLE) {
    float glow = 0.5f + 0.5f * sinf(t * 0.00785f);
    int gr = 31 + (int)(4.0f * glow);
    u->drawCircle(cx, cy, gr);
  }

  // ── ANTENNAE ──────────────────────────────────────────────
  int lwiggle = 0, rwiggle = 0;
  if (state == STATE_LISTENING) {
    lwiggle = (int)(5.0f * sinf(t * 0.01257f));
    rwiggle = (int)(5.0f * sinf(t * 0.01257f + 1.6f));
  }
  // Left
  u->drawLine(cx - 9, cy - 26, cx - 16 + lwiggle, cy - 43);
  u->drawDisc(cx - 16 + lwiggle, cy - 44, 3);
  // Right — Y-fork
  u->drawLine(cx + 9, cy - 25, cx + 17 + rwiggle, cy - 40);
  u->drawLine(cx + 17 + rwiggle, cy - 40, cx + 22 + rwiggle, cy - 35);
  u->drawLine(cx + 17 + rwiggle, cy - 40, cx + 22 + rwiggle, cy - 45);

  // ── BODY ──────────────────────────────────────────────────
  u->drawCircle(cx, cy, 28);

  // Panel arcs
  for (int dx = -18; dx <= 18; dx += 2)
    u->drawPixel(cx + dx, cy - 18);
  for (int dx = -18; dx <= 18; dx += 2)
    u->drawPixel(cx + dx, cy + 18);
  u->drawVLine(cx, cy - 27, 54);

  // ── EYE HOUSING ───────────────────────────────────────────
  u->drawDisc(cx, cy, 15);
  u->setDrawColor(0);
  u->drawDisc(cx, cy, 12);
  u->setDrawColor(1);

  // Iris + pupil
  u->drawCircle(cx, cy, 9);
  int pupilX = cx;
  if (state == STATE_IDLE)
    pupilX = cx + (int)(4.0f * sinf(t * 0.00209f));
  u->drawDisc(pupilX, cy, 5);

  // Specular highlight
  u->setDrawColor(0);
  u->drawPixel(pupilX - 2, cy - 2);
  u->drawPixel(pupilX - 1, cy - 2);
  u->setDrawColor(1);

  // ── SIDE SENSOR PODS ──────────────────────────────────────
  u->drawCircle(cx - 29, cy, 5);
  u->drawDisc  (cx - 29, cy, 3);
  u->drawCircle(cx + 29, cy, 5);
  u->drawDisc  (cx + 29, cy, 3);

  // ── THRUSTER RING ─────────────────────────────────────────
  u->drawEllipse(cx, cy + 29, 13, 4);
  u->drawLine(cx - 13, cy + 26, cx - 18, cy + 36);
  u->drawLine(cx + 13, cy + 26, cx + 18, cy + 36);
  u->drawLine(cx,      cy + 25, cx,      cy + 36);
}

// ── Main display render ───────────────────────────────────────
void renderDisplay() {
  U8G2* u = lcd.getU8g2();
  u->clearBuffer();

  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);

  const char* DAYS[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  const char* MONS[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                         "JUL","AUG","SEP","OCT","NOV","DEC"};

  // ╔═══════════════════════════════════════════════════════════╗
  // ║  HEADER BAR (filled black, white text)                   ║
  // ╚═══════════════════════════════════════════════════════════╝
  u->drawBox(0, 0, RLCD_WIDTH, HEADER_H);
  u->setDrawColor(0);
  // Brand name
  u->setFont(u8g2_font_profont17_mf);
  u->drawStr(4, 16, "THE PENCREUS");
  // Subtitle
  u->setFont(u8g2_font_profont10_mf);
  u->drawStr(148, 14, "//  PERSONAL STATUS MONITOR");

  // ── Battery indicator (right side of header) ──────────────
  // Icon body: 26×12 at x=367, y=5. Nib: 3×6 at x=393, y=8.
  // Fill bar: up to 22px inside body (2px padding each side).
  {
    char batBuf[6];
    if (batteryLevel == 255) {
      snprintf(batBuf, sizeof(batBuf), "---");
    } else {
      snprintf(batBuf, sizeof(batBuf), "%d%%", batteryLevel);
    }
    u->setFont(u8g2_font_profont10_mf);
    int btw = u->getStrWidth(batBuf);
    u->drawStr(363 - btw, 15, batBuf);   // right-align text to 4px before body
    u->drawFrame(367, 5, 26, 12);        // battery outline
    u->drawBox(393, 8, 3, 6);            // nib
    if (batteryLevel != 255 && batteryLevel > 0) {
      int fw = batteryLevel * 22 / 100;
      u->drawBox(369, 7, fw, 8);         // fill bar
    }
  }
  u->setDrawColor(1);

  // ╔═══════════════════════════════════════════════════════════╗
  // ║  ZONE 1 — DATE (left 128px) + CLOCK (right 272px)        ║
  // ║  Height: 78px                                            ║
  // ╚═══════════════════════════════════════════════════════════╝
  int z1_top = HEADER_H + 1;

  // Date column
  u->setFont(u8g2_font_profont29_mf);
  u->drawStr(4, z1_top + 26, DAYS[ti.tm_wday]);

  u->setFont(u8g2_font_profont12_mf);
  char dateBuf[20];
  snprintf(dateBuf, sizeof(dateBuf), "%02d %s %04d",
           ti.tm_mday, MONS[ti.tm_mon], ti.tm_year + 1900);
  u->drawStr(4, z1_top + 42, dateBuf);

  u->setFont(u8g2_font_profont10_mf);
  u->drawStr(4, z1_top + 56, ntpSynced ? "NTP SYNC" : "RTC ONLY");

  // Vertical divider
  u->drawVLine(128, z1_top, 78);

  // Clock
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
           ti.tm_hour, ti.tm_min, ti.tm_sec);
  u->setFont(u8g2_font_logisoso32_tf);
  int tw = u->getStrWidth(timeBuf);
  u->drawStr(128 + (272 - tw) / 2, z1_top + 44, timeBuf);

  u->setFont(u8g2_font_profont11_mf);
  const char* tzLabel = "LOCAL  PST8PDT";
  int tlw = u->getStrWidth(tzLabel);
  u->drawStr(128 + (272 - tlw) / 2, z1_top + 58, tzLabel);

  // ── DIVIDER ────────────────────────────────────────────────
  int z2_top = z1_top + 79;
  u->drawHLine(0, z2_top - 1, RLCD_WIDTH);

  // ╔═══════════════════════════════════════════════════════════╗
  // ║  ZONE 2 — OUTDOOR WEATHER                                ║
  // ║  Height: 76px                                            ║
  // ╚═══════════════════════════════════════════════════════════╝
  u->setFont(u8g2_font_profont10_mf);
  u->drawStr(4, z2_top + 8, "// OUTDOOR WEATHER  [OWM]");

  if (weather.valid) {
    // Condition
    u->setFont(u8g2_font_profont15_mf);
    char cond[34];
    snprintf(cond, sizeof(cond), "%s", weather.description);
    titleCase(cond);
    u->drawStr(4, z2_top + 22, cond);

    // Temperature (big)
    u->setFont(u8g2_font_logisoso24_tf);
    char tempBuf[12];
    snprintf(tempBuf, sizeof(tempBuf), "%.0f\xb0""F", weather.tempF);
    u->drawStr(4, z2_top + 52, tempBuf);

    // Hi / Lo / Sunrise / Sunset on one line
    u->setFont(u8g2_font_profont10_mf);
    char subBuf[64];
    char sr[8] = "??:??", ss[8] = "??:??";
    if (weather.sunrise) fmtTime12(weather.sunrise, sr, sizeof(sr));
    if (weather.sunset)  fmtTime12(weather.sunset,  ss, sizeof(ss));
    snprintf(subBuf, sizeof(subBuf),
             "H:%.0f L:%.0f   SR:%s  SS:%s",
             weather.tempMaxF, weather.tempMinF, sr, ss);
    u->drawStr(4, z2_top + 64, subBuf);

    // Right column: humidity + feels like
    u->drawVLine(300, z2_top + 8, 66);
    u->setFont(u8g2_font_profont10_mf);
    u->drawStr(305, z2_top + 16, "HUMIDITY");
    u->setFont(u8g2_font_profont17_mf);
    char humBuf[10];
    snprintf(humBuf, sizeof(humBuf), "%d%%", weather.humidity);
    u->drawStr(305, z2_top + 33, humBuf);

    u->setFont(u8g2_font_profont10_mf);
    u->drawStr(305, z2_top + 46, "FEELS");
    u->setFont(u8g2_font_profont17_mf);
    char feelBuf[12];
    snprintf(feelBuf, sizeof(feelBuf), "%.0f\xb0""F", weather.feelsF);
    u->drawStr(305, z2_top + 63, feelBuf);

  } else {
    u->setFont(u8g2_font_profont12_mf);
    u->drawStr(4, z2_top + 30, weatherFetched ? "NO DATA" : "FETCHING...");
  }

  // ── DIVIDER ────────────────────────────────────────────────
  int z3_top = z2_top + 77;
  u->drawHLine(0, z3_top - 1, RLCD_WIDTH);

  // ╔═══════════════════════════════════════════════════════════╗
  // ║  ZONE 3 — INDOOR SENSOR (left 160px) + ORB (right 240px) ║
  // ║  Height: 108px                                           ║
  // ╚═══════════════════════════════════════════════════════════╝
  u->setFont(u8g2_font_profont10_mf);
  u->drawStr(4, z3_top + 9, "// INTERIOR  [SHTC3]");

  if (indoor.valid) {
    u->setFont(u8g2_font_profont10_mf);
    u->drawStr(4, z3_top + 26, "TEMP");
    u->setFont(u8g2_font_logisoso20_tf);
    char inTBuf[12];
    snprintf(inTBuf, sizeof(inTBuf), "%.1f\xb0""F", indoor.tempF);
    u->drawStr(4, z3_top + 50, inTBuf);

    u->setFont(u8g2_font_profont10_mf);
    u->drawStr(4, z3_top + 64, "RH");
    u->setFont(u8g2_font_logisoso20_tf);
    char inHBuf[10];
    snprintf(inHBuf, sizeof(inHBuf), "%.1f%%", indoor.humidity);
    u->drawStr(4, z3_top + 88, inHBuf);
  } else {
    u->setFont(u8g2_font_profont11_mf);
    u->drawStr(4, z3_top + 40, "SENSOR");
    u->drawStr(4, z3_top + 55, "INIT...");
  }

  // Vertical divider before orb zone
  u->drawVLine(160, z3_top, 108);

  // Orb centered in right 240px × 108px cell
  // cy offset 50 keeps antennas clear of top divider
  drawOrb(u, 285, z3_top + 52, dispState);

  // ╔═══════════════════════════════════════════════════════════╗
  // ║  STATUS BAR — thin strip                                 ║
  // ╚═══════════════════════════════════════════════════════════╝
  int sb_top = z3_top + 109;
  u->drawHLine(0, sb_top, RLCD_WIDTH);

  u->setFont(u8g2_font_profont10_mf);
  const char* stateStr =
    (dispState == STATE_LISTENING)  ? "LISTENING" :
    (dispState == STATE_RESPONDING) ? "TRANSMIT"  : "STANDBY";
  char statusBuf[96];
  snprintf(statusBuf, sizeof(statusBuf),
           "WIFI:%s  MQ:%s  %s  %s  STATUS:%s",
           wifiConnected ? "OK" : "NC",
           mqttConnected ? "OK" : "NC",
           wifiConnected ? WiFi.SSID().c_str() : "---",
           stateStr,
           weather.valid ? "NOMINAL" : "DEGRADED");
  u->drawStr(2, sb_top + 10, statusBuf);

  u->sendBuffer();
}

// ── Splash screen ─────────────────────────────────────────────
// Full-canvas brand moment. Character left, title + boot log right.
// Called multiple times during boot — each call updates the boot message.
void renderSplash(const char* bootMsg) {
  U8G2* u = lcd.getU8g2();
  u->clearBuffer();

  // Double border frame
  u->drawFrame(2, 2, RLCD_WIDTH - 4, RLCD_HEIGHT - 4);
  u->drawFrame(5, 5, RLCD_WIDTH - 10, RLCD_HEIGHT - 10);

  // ── CHARACTER — left half, vertically centred ──────────────
  // SPLASH_LOGO_W=120, SPLASH_LOGO_H=154 → fits nicely left side
  int charX = 14;
  int charY = (RLCD_HEIGHT - SPLASH_LOGO_H) / 2;
  u->drawXBM(charX, charY, SPLASH_LOGO_W, SPLASH_LOGO_H, splash_logo_xbm);

  // ── RIGHT HALF: brand + boot log ──────────────────────────
  int rx = charX + SPLASH_LOGO_W + 12;   // ~146
  int rw = RLCD_WIDTH - rx - 14;         // ~240

  // Vertical rule separating char from text
  u->drawVLine(rx - 6, 14, RLCD_HEIGHT - 28);

  // Brand name — as large as profont allows, centred in right half
  u->setFont(u8g2_font_profont22_mf);
  const char* brand = "THE";
  int bw = u->getStrWidth(brand);
  u->drawStr(rx + (rw - bw) / 2, 50, brand);

  u->setFont(u8g2_font_profont22_mf);
  const char* brand2 = "PENCREUS";
  bw = u->getStrWidth(brand2);
  u->drawStr(rx + (rw - bw) / 2, 76, brand2);

  // Rule under brand
  u->drawHLine(rx, 82, rw);
  u->drawHLine(rx, 85, rw);

  // Boot log — mono small
  u->setFont(u8g2_font_profont10_mf);
  u->drawStr(rx, 100, "> ROBCO OS v4.2.0");
  u->drawStr(rx, 113, "> SHTC3 SENSOR...OK");
  u->drawStr(rx, 126, "> PCF85063 RTC...OK");
  u->drawStr(rx, 139, "> ST7305 LCD.....OK");

  // Dynamic boot message (WiFi / NTP status)
  if (bootMsg && *bootMsg) {
    u->drawStr(rx, 152, bootMsg);
  }

  // Progress bar at bottom of right column
  u->drawFrame(rx, 166, rw, 6);

  u->sendBuffer();
}

// ── Sensor read ───────────────────────────────────────────────
void readSensors() {
  if (!shtc3) return;
  float tempC = 0, rh = 0;
  shtc3->Shtc3_ReadTempHumi(&tempC, &rh);
  indoor.tempF    = tempC * 9.0f / 5.0f + 32.0f;
  indoor.humidity = rh;
  indoor.valid    = true;
}

// ── Weather fetch ─────────────────────────────────────────────
void fetchWeather() {
  if (!wifiConnected) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "http://api.openweathermap.org/data/2.5/weather"
    "?lat=%s&lon=%s&units=%s&appid=%s",
    OWM_LAT, OWM_LON, OWM_UNITS, OWM_API_KEY);

  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, payload)) {
      strlcpy(weather.description,
              doc["weather"][0]["description"] | "unknown",
              sizeof(weather.description));
      weather.tempF    = doc["main"]["temp"]       | 0.0f;
      weather.tempMinF = doc["main"]["temp_min"]   | 0.0f;
      weather.tempMaxF = doc["main"]["temp_max"]   | 0.0f;
      weather.feelsF   = doc["main"]["feels_like"] | 0.0f;
      weather.humidity = doc["main"]["humidity"]   | 0;
      weather.sunrise  = (time_t)(doc["sys"]["sunrise"] | 0);
      weather.sunset   = (time_t)(doc["sys"]["sunset"]  | 0);
      weather.valid    = true;
      weatherFetched   = true;
    }
  }
  http.end();
  lastWeatherMs = millis();
}

// ── WiFi connect ──────────────────────────────────────────────
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(250);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
}

// ── NTP sync ──────────────────────────────────────────────────
void syncNTP() {
  if (!wifiConnected) return;
  configTzTime(TZ_POSIX, NTP_SERVER, NTP_SERVER2);
  struct tm ti;
  unsigned long t = millis();
  while (!getLocalTime(&ti, 1000) && millis() - t < 5000) delay(100);
  ntpSynced = (ti.tm_year > 100);
  if (ntpSynced) {
    Rtc_SetTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
  }
}

// ── setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // KEY button — GPIO 0 (BOOT), active LOW, with debounce in loop
  pinMode(KEY_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(KEY_PIN), onKeyPress, FALLING);

  // RTC — seed system time before NTP arrives
  Rtc_Setup(&i2cBus, RTC_I2C_ADDR);
  rtcTimeStruct_t rtcT;
  Rtc_GetTime(&rtcT);
  struct tm ti = {};
  ti.tm_year = rtcT.year - 1900;
  ti.tm_mon  = rtcT.month - 1;
  ti.tm_mday = rtcT.day;
  ti.tm_hour = rtcT.hour;
  ti.tm_min  = rtcT.minute;
  ti.tm_sec  = rtcT.second;
  time_t epoch = mktime(&ti);
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  setenv("TZ", TZ_POSIX, 1);
  tzset();

  // Display init
  lcd.begin(0, U8G2_R1);   // R1 → landscape 400×300

  // SHTC3 sensor
  shtc3 = new Shtc3Port(i2cBus);

  // Splash + connect
  renderSplash("> CONNECTING TO VAULT-NET...");
  connectWiFi();
  renderSplash(wifiConnected ? "> WIFI OK. SYNCING NTP..." : "> WIFI FAILED. RTC MODE.");
  syncNTP();

  // OTA — advertise on mDNS so arduino-cli can find us by hostname
  if (wifiConnected) {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.begin();
  }

  // MQTT — connect right after WiFi is up
  connectMQTT();

  // First data pull
  readBattery();
  readSensors();
  fetchWeather();
}

// ── loop ──────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── KEY button debounce + state cycle ─────────────────────
  if (keyPressed && (now - lastKeyMs > 300)) {
    keyPressed  = false;
    lastKeyMs   = now;
    dispState   = (DispState)((dispState + 1) % 3);
    Serial.printf("State → %s\n",
      dispState == STATE_IDLE       ? "IDLE" :
      dispState == STATE_LISTENING  ? "LISTENING" : "RESPONDING");
  }

  // ── WiFi watchdog ─────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    connectWiFi();
    if (wifiConnected && !ntpSynced) syncNTP();
  } else {
    wifiConnected = true;
  }

  // ── OTA handler ───────────────────────────────────────────
  ArduinoOTA.handle();

  // ── MQTT watchdog + pump ───────────────────────────────────
  if (wifiConnected) {
    mqttConnected = mqtt.connected();
    if (!mqttConnected && (now - lastMqttMs >= MQTT_RECONNECT_MS)) {
      connectMQTT();
      lastMqttMs = now;
    }
    mqtt.loop();   // must be called every loop to service incoming messages
  } else {
    mqttConnected = false;
  }

  // ── Battery update ────────────────────────────────────────
  if (now - lastBatteryMs >= INTERVAL_BATTERY) {
    readBattery();
    lastBatteryMs = now;
  }

  // ── Sensor update ─────────────────────────────────────────
  if (now - lastSensorMs >= INTERVAL_SENSOR) {
    readSensors();
    lastSensorMs = now;
  }

  // ── Weather update ────────────────────────────────────────
  if (now - lastWeatherMs >= INTERVAL_WEATHER) {
    fetchWeather();
  }

  // ── Display tick — every second for clock, every 50ms for orb anim ──
  uint32_t dispInterval = (dispState == STATE_IDLE) ? INTERVAL_CLOCK : 50;
  if (now - lastClockMs >= dispInterval) {
    renderDisplay();
    lastClockMs = now;
  }
}
