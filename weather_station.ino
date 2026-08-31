#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <string.h>
#include "epd_driver.h"
#include "utilities.h"   // BOARD_SDA / BOARD_SCL / TOUCH_INT for this board
#include "TouchDrv.hpp"  // GT911 touch driver (bundled with LilyGo EPD47's SensorLib dependency)
#include "config.h"
#include "models.h"
#include "weather.h"
#include "weather_ec.h"
#include "weather_swob.h"
#include "display.h"
#include "renderer.h"

static const char* TZ_STRING  = "EST5EDT,M3.2.0,M11.1.0"; // Sudbury, ON — handles EST/EDT
static const char* NTP_SERVER = "pool.ntp.org";

enum Page { PAGE_HOME, PAGE_DAILY };
static Page currentPage = PAGE_HOME;

static TouchDrvGT911 touch;
static bool touchReady = false;

static WeatherData currentWeather;                // last known-good, never wiped on failure
static unsigned long lastWeatherAttemptMs = 0;
static const unsigned long WEATHER_INTERVAL_MS =
    (unsigned long)WEATHER_REFRESH_MINUTES * 60UL * 1000UL;  // background check cadence (config.h)

// Partial refresh never fully "cleans" the panel the way a full clear does —
// that's a physical e-ink trait, not a bug — so ghosting can slowly build up
// over many hours of partial-only updates. Every few hours we do one normal
// full refresh (not the library's ~30s screen_repair flash cycle, which is
// a heavier manual fix for when ghosting has already gotten bad) just to
// keep the panel clean.
static unsigned long lastFullRefreshMs = 0;
static const unsigned long FULL_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // every 6 hours

static int  lastRenderedMinute = -1;      // guards against firing twice in the same :01 window
static char lastDrawnDateStr[32] = "";    // only redraw the date when this actually changes

// Combines THREE independent sources into one WeatherData -- see the
// comment above the struct in models.h for which fields each one owns:
//   - SWOB (weather_swob.cpp): temperature/feels-like/humidity/wind/
//     lastUpdated. Raw per-minute station telemetry -- this is what makes
//     the home screen's numbers actually refresh on this project's
//     10-minute check cadence, instead of only changing once an hour.
//   - EC citypage (weather_ec.cpp): condition text + icon bucket. Only
//     source with a human-written summary ("Mostly Cloudy" etc.), so it's
//     kept even though its own feed only updates hourly.
//   - Open-Meteo (weather.cpp): daily high/low/precip/sunrise-sunset, and
//     isDay.
// All three calls are independent -- any can fail without blanking what
// the others already know, because each is seeded from the LAST KNOWN
// GOOD struct before being overwritten. Field ownership is deliberately
// explicit here rather than trusting any fetch to leave the others' fields
// alone: fetchWeather() (Open-Meteo) still fully populates its OWN struct,
// current-conditions fields included, but only its DAILY fields get copied
// out below -- its current-conditions half is simply discarded (SWOB/EC
// own that now).
// Returns true if at least one of the three calls succeeded; `valid` is
// set here (not by any individual fetch function) for the same reason --
// it means "out has at least one good field in it", which is a property
// of the merge, not of any single source.
static bool fetchAllWeather(WeatherData &out) {
  WeatherData merged = out;
  bool swobOk = fetchCurrentConditionsSWOB(merged); // owns: temp/feelsLike/humidity/wind/lastUpdated
  bool ecOk   = fetchCurrentConditionsEC(merged);   // owns: weatherCode/conditionText

  WeatherData omResult = out;
  bool omOk = fetchWeather(omResult);
  if (omOk) {
    merged.tempMaxC = omResult.tempMaxC;
    merged.tempMinC = omResult.tempMinC;
    merged.precipProbabilityMax = omResult.precipProbabilityMax;
    merged.windMaxKph = omResult.windMaxKph;
    strncpy(merged.sunrise, omResult.sunrise, sizeof(merged.sunrise) - 1);
    merged.sunrise[sizeof(merged.sunrise) - 1] = '\0';
    strncpy(merged.sunset, omResult.sunset, sizeof(merged.sunset) - 1);
    merged.sunset[sizeof(merged.sunset) - 1] = '\0';
    merged.isDay = omResult.isDay;
  }

  if (!swobOk && !ecOk && !omOk) return false;
  merged.valid = true;
  out = merged;
  return true;
}

static void connectWiFi() {
  Serial.printf("[wifi] connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[wifi] connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] connect timed out; will keep retrying in loop()");
  }
}

static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  connectWiFi(); // covers "WiFi temporarily unavailable" — retries, doesn't hard-fault
}

// Fills timeStr ("10:57 PM"), dateStr ("Friday, Aug 28, 2026"), tzStr ("EDT")
// from a given, already-fetched struct tm.
static void formatClockStrings(const struct tm &timeinfo,
                                char* timeStr, size_t timeLen,
                                char* dateStr, size_t dateLen,
                                char* tzStr, size_t tzLen) {
  strftime(timeStr, timeLen, "%I:%M %p", &timeinfo);  // 12-hour, no seconds
  strftime(dateStr, dateLen, "%A, %b %d, %Y", &timeinfo);
  strftime(tzStr, tzLen, "%Z", &timeinfo);             // e.g. "EDT" / "EST"
}

// Redraws time (always) and date (only if it changed) via partial refresh.
static void refreshClockDisplay(const struct tm &timeinfo) {
  char timeStr[16], dateStr[32], tzStr[8];
  formatClockStrings(timeinfo, timeStr, sizeof(timeStr), dateStr, sizeof(dateStr), tzStr, sizeof(tzStr));

  Renderer::drawTimePartial(timeStr, tzStr);

  if (strcmp(dateStr, lastDrawnDateStr) != 0) {
    Renderer::drawDatePartial(dateStr);
    strncpy(lastDrawnDateStr, dateStr, sizeof(lastDrawnDateStr) - 1);
    lastDrawnDateStr[sizeof(lastDrawnDateStr) - 1] = '\0';
  }
}

// Same GT911 bring-up sequence the vendor's own examples use for this board:
// scan both possible I2C addresses, then configure touch coordinates to
// line up with our 960x540 framebuffer.
static void initTouch() {
  Wire.begin(BOARD_SDA, BOARD_SCL);
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, HIGH);

  uint8_t touchAddress = 0;
  Wire.beginTransmission(0x14);
  if (Wire.endTransmission() == 0) touchAddress = 0x14;
  Wire.beginTransmission(0x5D);
  if (Wire.endTransmission() == 0) touchAddress = 0x5D;

  if (touchAddress == 0) {
    Serial.println("[touch] GT911 not found on the bus - touch navigation disabled");
    return;
  }

  touch.setPins(-1, TOUCH_INT);
  if (!touch.begin(Wire, touchAddress, BOARD_SDA, BOARD_SCL)) {
    Serial.println("[touch] begin() failed - touch navigation disabled");
    return;
  }
  touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT);
  touch.setSwapXY(true);
  touch.setMirrorXY(false, true);
  touchReady = true;
  Serial.println("[touch] GT911 ready");
}

// Full clear + full redraw of the whole panel, using whatever data we have
// right now. Used once at boot, and again periodically to clear ghosting.
static void doFullRefresh() {
  struct tm timeinfo;
  char timeStr[16], dateStr[32], tzStr[8];
  if (getLocalTime(&timeinfo, 1000)) {
    formatClockStrings(timeinfo, timeStr, sizeof(timeStr), dateStr, sizeof(dateStr), tzStr, sizeof(tzStr));
    lastRenderedMinute = timeinfo.tm_min; // don't immediately re-fire at the next :01
  } else {
    strcpy(timeStr, "--:--");
    strcpy(dateStr, "unsynced");
    tzStr[0] = '\0';
  }
  strncpy(lastDrawnDateStr, dateStr, sizeof(lastDrawnDateStr) - 1);
  lastDrawnDateStr[sizeof(lastDrawnDateStr) - 1] = '\0';

  Renderer::drawFullScreen(currentWeather, timeStr, dateStr, tzStr);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[main] booting");

  if (!Display::init()) {
    Serial.println("[main] display init failed, halting");
    while (true) delay(1000);
  }

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_STRING, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      Serial.println("[main] NTP time synced");
    } else {
      Serial.println("[main] NTP sync failed, time will show as 'unsynced' for now");
    }
  }

  fetchAllWeather(currentWeather); // best effort; screen below shows "waiting..." if it fails

  // One full-screen draw at boot: chrome (dividers/labels) + initial values.
  // Deliberately done BEFORE touch init: this is the core feature, and it
  // must show up even if touch (new, can't be tested off real hardware)
  // ever misbehaves during its own setup.
  doFullRefresh();

  unsigned long now = millis();
  lastWeatherAttemptMs = now;
  lastFullRefreshMs = now;

  initTouch(); // best-effort; touch navigation just stays disabled if this fails
}

// Tapping the weather area on the home screen opens the daily overview;
// tapping "< Back" there returns home. Both are full-screen redraws (the
// daily page is a completely different layout, not a partial update).
//
// PORTRAIT MODE: the touch controller still reports raw coordinates in the
// panel's native 960x540 addressing (that's what setMaxCoordinates was
// configured for) — but the renderer's hit-test functions expect PORTRAIT
// coordinates (px 0..539, py 0..959), matching how content is now laid out.
// This is the exact inverse of the renderer's placement transform
// (nativeX=py, nativeY=539-px), so: px = 539 - nativeY, py = nativeX.
static void handleTouch() {
  if (!touchReady) return;

  int16_t rawX, rawY;
  if (!touch.getPoint(&rawX, &rawY, 1)) return;

  int32_t x = 539 - rawY; // portrait px
  int32_t y = rawX;       // portrait py

  if (currentPage == PAGE_HOME && Renderer::isHomeScreenWeatherTap(x, y)) {
    Serial.println("[touch] weather tapped -> daily overview");
    struct tm timeinfo;
    char dateStr[32] = "unsynced";
    if (getLocalTime(&timeinfo, 200)) {
      strftime(dateStr, sizeof(dateStr), "%A, %b %d, %Y", &timeinfo);
    }
    currentPage = PAGE_DAILY;
    Renderer::drawDailyScreen(currentWeather, dateStr);

  } else if (currentPage == PAGE_DAILY && Renderer::isDailyPageBackButtonTap(x, y)) {
    Serial.println("[touch] back tapped -> home");
    currentPage = PAGE_HOME;
    doFullRefresh(); // redraw home with whatever's current now
  }
}

void loop() {
  ensureWiFi();
  handleTouch();

  unsigned long now = millis();

  // Weather is CHECKED in the background every WEATHER_REFRESH_MINUTES
  // regardless of which page is showing, but the panel is only actually
  // redrawn when a check comes back with something visibly different (see
  // weatherDisplayChanged()) — so checking often doesn't mean flashing the
  // e-ink often. Redraw itself only ever touches the HOME SCREEN; the daily
  // page is a point-in-time snapshot from when you opened it.
  if (now - lastWeatherAttemptMs >= WEATHER_INTERVAL_MS) {
    lastWeatherAttemptMs = now;

    // Seeded from currentWeather (not default-constructed): fetchAllWeather()
    // only overwrites the fields whichever underlying call(s) succeeded this
    // round, and needs a real "last known good" starting point to fall back
    // to for the other fields, same as it gets at the setup() call site
    // where `out` already IS currentWeather.
    WeatherData fetched = currentWeather;
    if (fetchAllWeather(fetched)) {
      bool changed = !currentWeather.valid || weatherDisplayChanged(fetched, currentWeather);
      currentWeather = fetched; // only overwrite on success
      if (changed && currentPage == PAGE_HOME) {
        Renderer::drawWeatherPartial(currentWeather);
      } else if (!changed) {
        Serial.println("[weather] no visible change since last check, skipping redraw");
      }
    } else {
      Serial.println("[main] weather fetch failed, keeping last known data");
    }
  }

  // Everything below only touches the home screen's own display, so skip
  // it while the daily page is showing — otherwise a minute-tick partial
  // refresh would scribble into daily-page screen space that means
  // something completely different there.
  if (currentPage == PAGE_HOME) {
    if (now - lastFullRefreshMs >= FULL_REFRESH_INTERVAL_MS) {
      lastFullRefreshMs = now;
      Serial.println("[main] periodic full refresh (anti-ghosting)");
      doFullRefresh();
    }

    // Clock tick: driven by the REAL clock's seconds, not a millis()
    // interval, so it can't drift based on when we booted. Fires once,
    // right as each new minute starts (:01), then waits for the next one.
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 200)) {
      if (timeinfo.tm_sec == 1 && timeinfo.tm_min != lastRenderedMinute) {
        lastRenderedMinute = timeinfo.tm_min;
        refreshClockDisplay(timeinfo);
      }
    }
  }

  delay(1000); // ~1x/sec is plenty to reliably catch touches and the :01 mark
}
