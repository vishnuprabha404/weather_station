#pragma once
#include "models.h"

// Fetches the human-readable CONDITION (e.g. "Mostly Cloudy", "Chance of
// Flurries") from Environment Canada's public MSC GeoMet API
// (api.weather.gc.ca, "citypageweather-realtime" collection) -- the same
// structured data that generates the weather.gc.ca city page for
// WEATHER_EC_SITE_ID (config.h).
//
// This USED to also own temperature/feels-like/humidity/wind/lastUpdated,
// but that citypage rollup only actually changes about once an HOUR (it's
// the station's official transmitted report, not a live feed), which was
// too stale for this project's 10-minute check cadence. Those numeric
// fields moved to fetchCurrentConditionsSWOB() (weather_swob.h), which
// reads the same station's raw per-MINUTE telemetry instead. This file's
// job shrank to just the free-text condition description + the icon
// bucket derived from it, since that text has no equivalent in the SWOB
// feed (SWOB is numeric sensor readings only, no human-written summary).
//
// Open-Meteo (weather.cpp / fetchWeather()) remains the source for the
// tap-through daily-overview page (high/low, precip chance, sunrise/
// sunset) and for isDay. See fetchAllWeather() in weather_station.ino for
// how all three sources are merged into one WeatherData.
//
// Returns false on ANY failure (WiFi down, timeout, bad JSON, missing
// fields) and leaves `out` untouched -- EXCEPT that on success it only
// overwrites the fields it owns (weatherCode, conditionText); every other
// field in `out` passes through untouched, since this seeds its working
// copy from `out` itself before overwriting only its own fields.
bool fetchCurrentConditionsEC(WeatherData &out);
