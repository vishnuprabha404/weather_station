#pragma once
#include <stdint.h>

// Three independent sources feed this struct (see fetchAllWeather() in
// weather_station.ino for the merge): SWOB (weather_swob.cpp) owns
// temperatureC/feelsLikeC/humidityPct/windSpeedKph/windDirectionDeg/
// lastUpdated -- raw per-MINUTE station telemetry, chosen specifically so
// this project's 10-minute check cadence actually gets ~10-minute-fresh
// data. EC citypage (weather_ec.cpp) owns weatherCode/conditionText -- the
// human-written condition summary, which SWOB's numeric telemetry has no
// equivalent for (that source only updates hourly, but a stale condition
// string like "Cloudy" is far less noticeable than a stale temperature).
// Open-Meteo (weather.cpp) owns isDay and the daily-summary fields below.
struct WeatherData {
  float temperatureC   = 0;
  float feelsLikeC     = 0;
  int   humidityPct    = 0;
  float windSpeedKph   = 0;
  int   windDirectionDeg = 0;
  int   weatherCode    = -1;        // WMO-bucket code, -1 = none yet -- drives ICON selection only
                                     // (iconForWeather() in renderer.cpp). Sourced from Environment
                                     // Canada's citypage feed, this is a bucket derived from their
                                     // condition text (see ecConditionToIconCode() in
                                     // weather_ec.cpp), not a real WMO code -- it just reuses the
                                     // same numeric buckets iconForWeather() already switches on.
  char  conditionText[40] = "";     // human-readable condition, e.g. "Mostly Cloudy" -- what's
                                     // actually drawn on screen (uppercased at render time).
                                     // Independent of weatherCode above: two different EC
                                     // condition strings can land in the same icon bucket
                                     // (e.g. "Cloudy" and "Overcast" both -> the plain cloud
                                     // icon) but must still be told apart for display-change
                                     // detection, so weatherDisplayChanged() compares this too.
  bool  isDay           = true;     // from Open-Meteo's is_day (itself derived from sunrise/sunset)
                                     // -- neither EC source includes this, so day/night is still
                                     // owned by the Open-Meteo half of the fetch.
  char  lastUpdated[16] = "never";  // "HH:MM" (24h) -- SWOB's per-minute timestamp, NOT EC
                                     // citypage's (which would only ever show a stale-looking
                                     // hourly time even though the panel refreshes far more often).
  bool  valid = false;              // true once we've parsed at least one good response from ANY
                                     // of the three sources -- set centrally by fetchAllWeather(),
                                     // not by any individual fetch function (see its comment).

  // Today's daily summary (for the tap-through "daily overview" page).
  float tempMaxC = 0;
  float tempMinC = 0;
  int   precipProbabilityMax = 0;   // %
  float windMaxKph = 0;
  char  sunrise[8] = "--:--";       // "HH:MM" (24h)
  char  sunset[8]  = "--:--";       // "HH:MM" (24h)
};
