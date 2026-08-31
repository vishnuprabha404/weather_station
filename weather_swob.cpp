#include "weather_swob.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

static const uint32_t HTTP_TIMEOUT_MS = 10000;

// Days from the Unix epoch (1970-01-01) for a given proleptic-Gregorian
// UTC calendar date -- Howard Hinnant's well-known constant-time
// civil-to-days formula. Used in place of timegm(), which this ESP32
// Arduino core (2.0.14/2.0.15) doesn't provide -- it's a common GNU/BSD
// libc extension, not part of the newlib build this toolchain ships.
// Self-contained on purpose: no libc timezone-database dependency at all.
// (Same formula as weather_ec.cpp used before its own timestamp handling
// was retired in favour of this file owning lastUpdated -- duplicated
// rather than shared across a two-function firmware, per this project's
// "avoid unnecessary abstraction" preference, but keep both in sync if
// this is ever revisited.)
static int64_t daysFromCivilUTC(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                          // [0, 399]
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;      // [0, 365]
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
  return era * 146097 + (int64_t)doe - 719468;
}

// SWOB's date_tm-value is UTC ("...Z"); the home screen's "Weather updated
// at" line wants local time. localtime_r() resolves against whatever TZ
// the .ino's configTzTime() call set at boot, same mechanism the clock
// display itself relies on.
static void utcIso8601ToLocalHHMM(const char* iso, char* out, size_t outLen) {
  int y, mo, d, h, mi, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
    time_t utcEpoch = (time_t)(daysFromCivilUTC(y, mo, d) * 86400LL + h * 3600 + mi * 60 + s);
    struct tm tmLocal;
    localtime_r(&utcEpoch, &tmLocal);
    strftime(out, outLen, "%H:%M", &tmLocal);
  } else {
    strncpy(out, "unknown", outLen - 1);
    out[outLen - 1] = '\0';
  }
}

// EC's official wind chill index formula (Environment Canada / MSC), valid
// only at <=10C air temp and >=4.8 km/h wind -- below that threshold wind
// chill isn't meaningful/calculated, same cutoff EC's own site uses.
// Falls back to plain temperature outside that range.
static float computeWindChillC(float tempC, float windKph) {
  if (tempC > 10.0f || windKph < 4.8f) return tempC;
  float v016 = powf(windKph, 0.16f);
  return 13.12f + 0.6215f * tempC - 11.37f * v016 + 0.3965f * tempC * v016;
}

// Mirrors weather_ec.cpp's old computeFeelsLikeC() shape (only show a
// distinct feels-like figure when it would actually read differently), but
// computes wind chill itself instead of trusting a precomputed API field --
// SWOB's raw telemetry doesn't hand us one.
static float computeFeelsLikeC(float tempC, float dewpointC, float windKph) {
  if (tempC <= 10.0f) {
    float wc = computeWindChillC(tempC, windKph);
    if (wc < tempC - 0.5f) return wc; // only if actually meaningfully colder
  }
  if (tempC >= 20.0f) {
    // Canadian humidex formula (Masterton & Richardson 1979), from dewpoint.
    float dewK = dewpointC + 273.16f;
    float e = 6.11f * expf(5417.7530f * (1.0f / 273.16f - 1.0f / dewK));
    float humidex = tempC + 0.5555f * (e - 10.0f);
    if (humidex > tempC + 0.5f) return humidex; // only if it actually feels muggier
  }
  return tempC;
}

bool fetchCurrentConditionsSWOB(WeatherData &out) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[weather_swob] skip fetch: WiFi not connected");
    return false;
  }

  // Server-side filter (tc_id-value) + sort + limit=1 means the response is
  // already just the single latest minute's reading -- unlike citypage's
  // ~90KB whole-city-page payload, this is a few KB, so a plain
  // getString()+deserializeJson() (no stream/filter tricks) is fine here.
  char url[220]; // measured actual length ~140 chars for a typical station id; generous headroom
  snprintf(url, sizeof(url),
    "https://api.weather.gc.ca/collections/swob-realtime/items?f=json&tc_id-value=%s&sortby=-date_tm-value&limit=1",
    WEATHER_SWOB_STATION_ID);

  Serial.print("[weather_swob] GET ");
  Serial.println(url);

  WiFiClientSecure client;
  // Same "quick-start fallback" trade-off as weather_ec.cpp/the future bus
  // API -- see that file's comment for the reasoning.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[weather_swob] http.begin() failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[weather_swob] HTTP GET failed, code=%d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[weather_swob] JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray features = doc["features"];
  if (features.isNull() || features.size() == 0) {
    // Not necessarily a real error -- e.g. WEATHER_SWOB_STATION_ID typo'd,
    // or (very rare) the station missed its latest minute entirely.
    Serial.println("[weather_swob] no features in response (check WEATHER_SWOB_STATION_ID?)");
    return false;
  }

  JsonObject props = features[0]["properties"];
  if (props.isNull() || !props["air_temp"].is<float>()) {
    Serial.println("[weather_swob] response missing expected fields");
    return false;
  }

  WeatherData parsed = out; // seed from last known good -- see weather_swob.h's contract:
                             // only the fields touched below get overwritten
  parsed.temperatureC     = props["air_temp"]                  | out.temperatureC;
  parsed.humidityPct      = props["rel_hum"]                   | out.humidityPct;
  parsed.windSpeedKph     = props["avg_wnd_spd_10m_pst1mt"]    | out.windSpeedKph;
  parsed.windDirectionDeg = (int)(props["avg_wnd_dir_10m_pst1mt"] | (float)out.windDirectionDeg);

  float dewpointC = props["dwpt_temp"] | parsed.temperatureC;
  parsed.feelsLikeC = computeFeelsLikeC(parsed.temperatureC, dewpointC, parsed.windSpeedKph);

  const char* ts = props["date_tm-value"] | "";
  utcIso8601ToLocalHHMM(ts, parsed.lastUpdated, sizeof(parsed.lastUpdated));

  out = parsed;
  Serial.printf("[weather_swob] OK: %.1fC (feels %.1fC), %d%% RH, %.1f km/h, updated %s\n",
    out.temperatureC, out.feelsLikeC, out.humidityPct, out.windSpeedKph, out.lastUpdated);
  return true;
}
