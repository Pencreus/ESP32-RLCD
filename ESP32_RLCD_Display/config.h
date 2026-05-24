#pragma once

// ── Display SPI Pins (from Waveshare schematic) ──────────────
#define RLCD_SCK_PIN   11
#define RLCD_MOSI_PIN  12
#define RLCD_DC_PIN     5
#define RLCD_CS_PIN    40
#define RLCD_RST_PIN   41
#define RLCD_WIDTH    400   // after R1 rotation (landscape)
#define RLCD_HEIGHT   300

// ── Buttons ───────────────────────────────────────────────────
#define KEY_PIN        0    // onboard KEY button (active LOW / BOOT pin)

// ── I2C Bus Pins ──────────────────────────────────────────────
#define I2C_SDA_PIN   14
#define I2C_SCL_PIN   13

// ── Peripheral I2C Addresses ──────────────────────────────────
#define RTC_I2C_ADDR   0x51   // PCF85063
#define SHTC3_I2C_ADDR 0x70   // SHTC3

// ── NTP / Timezone ────────────────────────────────────────────
#define NTP_SERVER     "pool.ntp.org"
#define NTP_SERVER2    "time.google.com"
// POSIX TZ string — Pacific Time with automatic DST
#define TZ_POSIX       "PST8PDT,M3.2.0,M11.1.0"

// ── Refresh Intervals (milliseconds) ─────────────────────────
#define INTERVAL_WEATHER  300000UL   // 5 min — OWM free tier ~60 req/min
#define INTERVAL_SENSOR    30000UL   // 30 sec
#define INTERVAL_CLOCK      1000UL   // 1 sec

// ── UI layout constants ───────────────────────────────────────
#define HEADER_H        22
#define ROW1_Y          23
#define ROW2_Y         102
#define ROW3_Y         179
#define STATUS_Y       287
