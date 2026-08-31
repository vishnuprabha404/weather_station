#include "weather_ec.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static const uint32_t HTTP_TIMEOUT_MS = 10000;

// Case-insensitive substring search. Not relying on strcasestr() -- it's a
// POSIX/BSD extension, not guaranteed present in every ESP32 Arduino core's
// libc, and this is a handful of lines either way.
static bool containsCI(const char* haystack, const char* needle) {
  size_t hn = strlen(haystack), nn = strlen(needle);
  if (nn == 0 || nn > hn) return nn == 0;
  for (size_t i = 0; i + nn <= hn; i++) {
    size_t j = 0;
    for (; j < nn; j++) {
      char a = haystack[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
    }
    if (j == nn) return true;
  }
  return false;
}

// Reduces EC's free-text condition (e.g. "Mostly Cloudy", "Chance of
// Flurries") into the same WMO-ish bucket codes iconForWeather() in
// renderer.cpp already switches on -- so EC-sourced weather reuses all of
// the existing icon-selection logic (and its day/night handling for the
// clear-sky bucket) completely unchanged. This is intentionally NOT trying
// to reproduce EC's own real icon-code table (0-44ish gif codes) -- text
// matching against EC's actual vocabulary is simpler and the buckets it
// needs to land in are coarse (7 icon categories) either way.
//
// Order matters: check the most specific phrases first, since e.g. a
// thunderstorm forecast also usually contains "rain".
static int ecConditionToIconCode(const char* condition) {
  if (containsCI(condition, "thunder")) return 95;
  if (containsCI(condition, "flurr") || containsCI(condition, "snow") || containsCI(condition, "ice pellet"))
    return 71;
  if (containsCI(condition, "freezing rain") || containsCI(condition, "drizzle") ||
      containsCI(condition, "rain") || containsCI(condition, "shower"))
    return 61;
  if (containsCI(condition, "fog") || containsCI(condition, "mist") ||
      containsCI(condition, "haze") || containsCI(condition, "smoke"))
    return 45;
  if (containsCI(condition, "clear") || containsCI(condition, "sunny")) return 0;
  if (containsCI(condition, "partly") || containsCI(condition, "mix of sun") ||
      containsCI(condition, "few clouds") || containsCI(condition, "increasing cloud"))
    return 2;
  return 3; // cloudy/overcast/anything unrecognized -- iconForWeather()'s own default too
}

bool fetchCurrentConditionsEC(WeatherData &out) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[weather_ec] skip fetch: WiFi not connected");
    return false;
  }

  char url[200]; // measured actual length ~95 chars for a typical site id; generous headroom
  snprintf(url, sizeof(url),
    "https://api.weather.gc.ca/collections/citypageweather-realtime/items/%s?f=json",
    WEATHER_EC_SITE_ID);

  Serial.print("[weather_ec] GET ");
  Serial.println(url);

  WiFiClientSecure client;
  // EC's cert chain isn't pinned here -- setInsecure() is the same
  // "quick-start fallback" this project's own docs already flagged for the
  // (still-unstarted) bus API's HTTPS needs. Acceptable for a home
  // dashboard pulling public, non-sensitive weather data over a trusted
  // home network; revisit if this firmware ever handles anything that
  // actually needs to verify who it's talking to.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[weather_ec] http.begin() failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    // covers "API unavailable" and rate-limit (429) -- same handling as weather.cpp
    Serial.printf("[weather_ec] HTTP GET failed, code=%d\n", httpCode);
    http.end();
    return false;
  }

  // This endpoint's full response is still ~85-90KB (the whole city page --
  // multi-day + hourly forecasts, warnings) even though only the condition
  // text is wanted now. The filter is narrowed to JUST that one leaf (not
  // the whole currentConditions block, now that temperature/humidity/wind/
  // etc. are SWOB's job -- see weather_swob.cpp) so ArduinoJson builds as
  // little DOM as possible, and parsing straight from the stream avoids
  // ever holding the ~90KB body in one contiguous allocation.
  JsonDocument filter;
  filter["properties"]["currentConditions"]["condition"] = true;

  JsonDocument doc;
  DeserializationError err =
    deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.print("[weather_ec] JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  const char* condition = doc["properties"]["currentConditions"]["condition"]["en"] | (const char*)nullptr;
  if (condition == nullptr) {
    Serial.println("[weather_ec] response missing expected fields");
    return false;
  }

  WeatherData parsed = out; // seed from last known good -- see weather_ec.h's contract:
                             // only conditionText/weatherCode get overwritten below
  strncpy(parsed.conditionText, condition, sizeof(parsed.conditionText) - 1);
  parsed.conditionText[sizeof(parsed.conditionText) - 1] = '\0';
  parsed.weatherCode = ecConditionToIconCode(condition);

  out = parsed;
  Serial.printf("[weather_ec] OK: \"%s\" (icon bucket %d)\n", out.conditionText, out.weatherCode);
  return true;
}
