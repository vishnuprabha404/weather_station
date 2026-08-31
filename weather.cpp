#include "weather.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static const uint32_t HTTP_TIMEOUT_MS = 10000;

bool fetchWeather(WeatherData &out) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[weather] skip fetch: WiFi not connected");
    return false;
  }

  char url[512]; // measured actual length ~372 chars; keep generous headroom
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
    "weather_code,wind_speed_10m,wind_direction_10m,is_day"
    "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
    "wind_speed_10m_max,sunrise,sunset"
    "&temperature_unit=celsius&wind_speed_unit=kmh&timezone=auto&forecast_days=1",
    WEATHER_LATITUDE, WEATHER_LONGITUDE);

  Serial.print("[weather] GET ");
  Serial.println(url);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("[weather] http.begin() failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    // covers "API unavailable" and rate-limit (429) — same handling either way
    Serial.printf("[weather] HTTP GET failed, code=%d\n", httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[weather] JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject current = doc["current"];
  if (current.isNull() || !current["temperature_2m"].is<float>()) {
    Serial.println("[weather] response missing expected fields");
    return false;
  }

  WeatherData parsed;
  parsed.temperatureC     = current["temperature_2m"]        | 0.0f;
  parsed.feelsLikeC       = current["apparent_temperature"]  | parsed.temperatureC;
  parsed.humidityPct      = current["relative_humidity_2m"]  | 0;
  parsed.windSpeedKph     = current["wind_speed_10m"]        | 0.0f;
  parsed.windDirectionDeg = current["wind_direction_10m"]    | 0;
  parsed.weatherCode      = current["weather_code"]          | -1;
  // is_day is Open-Meteo's own flag, computed from the real sunrise/sunset
  // for these coordinates — this is that "check sunrise/sunset" logic,
  // just done server-side instead of us re-deriving it from raw times.
  parsed.isDay            = (current["is_day"] | 1) != 0; // 1=day, 0=night

  const char* t = current["time"] | ""; // e.g. "2026-08-28T14:00"
  const char* tpos = strchr(t, 'T');
  strncpy(parsed.lastUpdated, tpos ? tpos + 1 : "unknown", sizeof(parsed.lastUpdated) - 1);
  parsed.lastUpdated[sizeof(parsed.lastUpdated) - 1] = '\0';

  // Daily summary — optional; if missing we just keep the struct defaults
  // rather than failing the whole fetch over it.
  JsonObject daily = doc["daily"];
  if (!daily.isNull()) {
    parsed.tempMaxC = daily["temperature_2m_max"][0] | parsed.temperatureC;
    parsed.tempMinC = daily["temperature_2m_min"][0] | parsed.temperatureC;
    parsed.precipProbabilityMax = daily["precipitation_probability_max"][0] | 0;
    parsed.windMaxKph = daily["wind_speed_10m_max"][0] | parsed.windSpeedKph;

    const char* sr = daily["sunrise"][0] | "";
    const char* srPos = strchr(sr, 'T');
    strncpy(parsed.sunrise, srPos ? srPos + 1 : "--:--", sizeof(parsed.sunrise) - 1);
    parsed.sunrise[sizeof(parsed.sunrise) - 1] = '\0';

    const char* ss = daily["sunset"][0] | "";
    const char* ssPos = strchr(ss, 'T');
    strncpy(parsed.sunset, ssPos ? ssPos + 1 : "--:--", sizeof(parsed.sunset) - 1);
    parsed.sunset[sizeof(parsed.sunset) - 1] = '\0';
  } else {
    Serial.println("[weather] no daily data in response (daily page will show today's current stats as fallback)");
  }

  parsed.valid = true;

  out = parsed;
  Serial.printf("[weather] OK: %.1fC (feels %.1fC), %d%% RH, %.1f km/h %s, code=%d, %s\n",
    out.temperatureC, out.feelsLikeC, out.humidityPct, out.windSpeedKph,
    windDirectionToCompass(out.windDirectionDeg), out.weatherCode,
    out.isDay ? "day" : "night");
  return true;
}

const char* windDirectionToCompass(int degrees) {
  static const char* dirs[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  int deg = ((degrees % 360) + 360) % 360; // normalize into 0..359
  int idx = ((deg + 22) / 45) % 8;         // 8 compass points, 45deg each
  return dirs[idx];
}

// Rounds v*scale to the nearest integer and compares -- e.g. scale=10 for
// "%.1f" precision, scale=1 for "%.0f" precision. Avoids pulling in
// <math.h> for a single lroundf() call.
static bool sameAtDisplayPrecision(float a, float b, float scale) {
  float sa = a * scale, sb = b * scale;
  long ra = (long)(sa >= 0 ? sa + 0.5f : sa - 0.5f);
  long rb = (long)(sb >= 0 ? sb + 0.5f : sb - 0.5f);
  return ra == rb;
}

bool weatherDisplayChanged(const WeatherData &a, const WeatherData &b) {
  if (!sameAtDisplayPrecision(a.temperatureC, b.temperatureC, 10.0f)) return true;  // "%.1fC"
  if (!sameAtDisplayPrecision(a.feelsLikeC, b.feelsLikeC, 1.0f)) return true;       // "%.0fC"
  if (a.humidityPct != b.humidityPct) return true;
  if (!sameAtDisplayPrecision(a.windSpeedKph, b.windSpeedKph, 10.0f)) return true;  // "%.1f km/h"
  if (strcmp(windDirectionToCompass(a.windDirectionDeg), windDirectionToCompass(b.windDirectionDeg)) != 0) return true;
  if (a.weatherCode != b.weatherCode) return true;
  // Two different condition strings can share an icon bucket (weatherCode) --
  // e.g. EC's "Cloudy" and "Overcast" both draw the plain cloud icon -- so
  // the icon-selection code alone isn't enough to catch every visible change.
  if (strcmp(a.conditionText, b.conditionText) != 0) return true;
  if (a.isDay != b.isDay) return true;
  return false;
}

void formatTime12h(const char* hhmm24, char* out, size_t outLen) {
  int hour = 0, minute = 0;
  if (sscanf(hhmm24, "%d:%d", &hour, &minute) != 2) {
    strncpy(out, hhmm24, outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }
  const char* ampm = (hour < 12) ? "AM" : "PM";
  int hour12 = hour % 12;
  if (hour12 == 0) hour12 = 12;
  snprintf(out, outLen, "%d:%02d %s", hour12, minute, ampm);
}
