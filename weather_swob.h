#pragma once
#include "models.h"

// Fetches CURRENT CONDITIONS from Environment Canada's SWOB (Surface
// Weather Observations) real-time feed (api.weather.gc.ca,
// "swob-realtime" collection) -- the raw per-MINUTE telemetry from the
// automated station identified by WEATHER_SWOB_STATION_ID (config.h),
// filtered/sorted server-side to just the single latest reading via
// `tc_id-value=<id>&sortby=-date_tm-value&limit=1`.
//
// Why this exists alongside fetchCurrentConditionsEC() (weather_ec.h):
// that function's source -- the "citypageweather-realtime" collection --
// is literally weather.gc.ca's own displayed number, but that number only
// changes when the station's official rollup transmits, which is roughly
// once an HOUR. This project's background check cadence
// (WEATHER_REFRESH_MINUTES, currently 10 min) was already that frequent;
// the citypage source just didn't have anything fresher to hand back most
// of the time. SWOB is the same physical station's raw sensor telemetry,
// timestamped and transmitted every ~1 minute (confirmed by querying it
// live and inspecting consecutive date_tm-value timestamps) -- so a check
// every 10 minutes now genuinely gets 10-minutes-old-or-less data instead
// of up-to-an-hour-old data, without needing a different, less-trustworthy
// data source.
//
// Field ownership: this is now the PRIMARY source for temperatureC,
// feelsLikeC, humidityPct, windSpeedKph, windDirectionDeg, and
// lastUpdated -- fetchCurrentConditionsEC() no longer touches any of
// those; its role has shrunk to just conditionText/weatherCode, since
// SWOB's raw telemetry has no human-readable condition description (no
// "Mostly Cloudy" -- it's numeric sensor readings only). Open-Meteo still
// separately owns the daily fields + isDay. See fetchAllWeather() in
// weather_station.ino for how all three are merged into one WeatherData.
//
// feelsLikeC is computed HERE from raw inputs (unlike
// fetchCurrentConditionsEC(), which used to trust a precomputed windChill
// value citypage handed it directly) -- SWOB has no such precomputed
// field, only air_temp/dwpt_temp/wind speed, so this file carries its own
// wind chill (EC's official formula) + humidex (from dewpoint) math.
//
// Returns false on ANY failure (WiFi down, timeout, bad JSON, missing
// fields, or the station simply hasn't reported yet) and leaves `out`
// untouched, same contract as the other fetch functions -- EXCEPT that on
// success it only overwrites the fields it owns (temperatureC, feelsLikeC,
// humidityPct, windSpeedKph, windDirectionDeg, lastUpdated); everything
// else in `out` passes through untouched, since this seeds its working
// copy from `out` itself before overwriting only its own fields.
bool fetchCurrentConditionsSWOB(WeatherData &out);
