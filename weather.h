#pragma once
#include <stddef.h>
#include "models.h"

// Fetches current weather from Open-Meteo and fills `out` on success.
// Returns false on ANY failure (WiFi down, timeout, bad JSON, missing fields)
// and leaves `out` untouched — caller decides what to do with stale data.
//
// As of the Environment Canada / SWOB integration (see weather_ec.h and
// weather_swob.h), this is only the DAILY half of the picture (tempMax/
// tempMin/precipProbabilityMax/windMaxKph/sunrise/sunset/isDay) — the
// caller (weather_station.ino's fetchAllWeather()) deliberately discards
// the current-conditions fields this also fills in, in favour of
// fetchCurrentConditionsSWOB()'s (temperature/feels-like/humidity/wind)
// and fetchCurrentConditionsEC()'s (condition text/icon). This function
// itself is unaware of that and still returns a fully-populated
// WeatherData, same as always.
bool fetchWeather(WeatherData &out);

// Converts a wind direction in degrees (0-360) into an 8-point compass label.
const char* windDirectionToCompass(int degrees);

// Converts a 24h "HH:MM" string (as returned by the API) into 12-hour
// "H:MM AM/PM" form. `out` must be at least 12 bytes.
void formatTime12h(const char* hhmm24, char* out, size_t outLen);

// True if the two snapshots would actually render differently on screen.
// Compares at DISPLAY precision, not raw API precision -- e.g. temperature
// is rounded to 1 decimal (matching "%.1fC" on screen) and wind direction
// is compared as its 8-point compass label (matching what's shown), not
// raw degrees -- so a fetch that changes only in ways too small to see
// doesn't count as a change. Used to skip a partial refresh when a
// scheduled fetch comes back effectively the same, so the panel isn't
// flashed/worn down for no visible reason.
bool weatherDisplayChanged(const WeatherData &a, const WeatherData &b);
