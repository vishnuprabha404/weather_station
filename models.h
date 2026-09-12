#pragma once
#include <stdint.h>
#include <time.h>

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

// --- Bus data (GOVA Transit, Greater Sudbury -- via Consat/tmix's GTFS
// feeds) -----------------------------------------------------------------
// Ported from the user's existing bus-stop project (backend/gova_next_bus.py
// and its Kotlin/GNOME ports) rather than a new pipeline built from scratch,
// per this project's original "reuse existing bus-data logic" goal -- see
// CLAUDE.md's "Real-Time Bus Data" section and clauderef.md for the full
// fetch/parse/merge strategy this feeds. bus.cpp owns the merge;
// bus_static.cpp/bus_realtime.cpp own their one source each, same
// one-file-per-source split as weather.cpp/weather_ec.cpp/weather_swob.cpp.

#define BUS_MAX_ROUTES_PER_STOP   4
#define BUS_MAX_ARRIVALS_PER_STOP 3

// One configured stop to show on the dashboard: the next N buses serving
// ANY of `routes` at `stopId`, merged and sorted together -- same shape as
// gova_next_bus.py's STOPS list / the Kotlin port's Config.kt. Real values
// live in config.h (gitignored), not here or in bus.cpp -- like
// WEATHER_LATITUDE/LONGITUDE, a stop_id is location-identifying (which
// street corner you actually catch a bus at), so it doesn't belong
// hardcoded into a file this now-public repo ships.
struct BusStopConfig {
  const char* stopId;
  const char* routes[BUS_MAX_ROUTES_PER_STOP]; // unused trailing slots are "" -- see routeCount
  int routeCount;
  const char* label;
};

// One upcoming bus at one configured stop, already reduced to what a
// renderer would want -- no protobuf/CSV/HTTP types leak out of bus.cpp's
// merge step into this struct, same separation weather.cpp/renderer.cpp
// already keep.
struct BusArrival {
  char   route[16]      = "";    // e.g. "11", "1N" -- GOVA's are short; generous headroom for other agencies
  char   headsign[32]   = "";    // trip destination text -- ALWAYS from the static schedule (the realtime
                                  // feed carries no headsign field at all, only the static one does)
  int    minutes         = 0;     // rounded ETA in minutes, clamped >= 0 -- see formatBusEta()
  bool   live             = false; // true = backed by the realtime feed for THIS trip, false = static-schedule-only guess
  int    delayMin         = 0;     // +late / -early vs. schedule, from the realtime feed; 0 if unknown/on-time
  bool   hasScheduledTime = false; // true if a static-schedule baseline time ALSO exists for this same
                                   // trip+day (see bus.cpp's merge) -- lets a renderer show both the
                                   // original scheduled clock time and the live delay together, rather
                                   // than the live update silently overwriting the scheduled time
  time_t epoch            = 0;     // the time actually used for sorting/ETA (live if available, else scheduled)
  time_t scheduledEpoch   = 0;     // only meaningful when hasScheduledTime is true
};

struct BusStopResult {
  char stopId[16] = "";
  char label[40]  = "";
  BusArrival arrivals[BUS_MAX_ARRIVALS_PER_STOP];
  int  arrivalCount = 0;
  bool realtimeOk   = false; // true if tripupdates.pb was reachable AND fresh THIS cycle -- independent of
                             // whether it had anything for THIS particular stop (see bus_realtime.h)
  bool hasAnyData   = false; // true if EITHER the realtime feed or the static schedule contributed
                             // anything at all for this stop -- what a renderer should check before
                             // showing "no data" instead of an empty arrivals list
};
