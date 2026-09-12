#pragma once
#include "models.h"
#include <time.h>

// One trip belonging to one of the routes this project watches (config.h's
// BUS_STOPS) -- a small, filtered subset of GOVA's whole trips.txt, not the
// whole file. See ensureStaticSchedule() in bus_static.cpp for the
// single-pass-per-CSV filtering strategy this relies on -- the same one
// gova_next_bus.py's load_static_data() uses, and what CLAUDE.md/
// clauderef.md call out as the RAM-appropriate approach here: a whole
// city's schedule would not fit, but this project's watched routes are a
// handful, so the filtered result is small.
struct StaticTrip {
  char tripId[24];
  char route[16];
  char headsign[32]; // may be empty -- trip_headsign is optional in GTFS
  char serviceId[16];
};

// One stop_times.txt row, for a trip already known to be on one of our
// watched routes, at one of our watched stop_ids.
struct StaticStopTime {
  char tripId[24];
  char stopId[16];
  int  secondsSinceMidnight; // parsed from "HH:MM:SS" -- HH can be >=24 for post-midnight service
};

// One calendar_dates.txt row with exception_type=1 (service ADDED on this
// date), for a service_id one of our watched trips actually uses. GOVA
// (like most small agencies) publishes calendar_dates.txt rather than a
// day-of-week calendar.txt pattern. Only ADDED rows are kept -- REMOVED
// (exception_type=2) rows are never seen at all by this project, same
// simplification gova_next_bus.py's service_dates dict carries (it only
// ever adds dates, never subtracts a removal), which is fine in practice
// since small agencies overwhelmingly use calendar_dates.txt as a plain
// "service runs on these specific dates" list rather than combining it
// with calendar.txt exceptions.
struct StaticServiceDate {
  char serviceId[16];
  char date[9]; // "YYYYMMDD"
};

#define BUS_STATIC_MAX_TRIPS         200
#define BUS_STATIC_MAX_STOP_TIMES    500
#define BUS_STATIC_MAX_SERVICE_DATES 128

// ~42KB fully populated (200*88 + 500*44 + 128*25 bytes) -- NOT safe as a
// plain stack-local variable (default Arduino task stacks are far smaller
// than that). Callers (bus.cpp) must hold this as a `static` or global, not
// a local declared inside a function -- ensureStaticSchedule() itself does
// the same internally for its own working copy (see that function).
struct StaticSchedule {
  StaticTrip trips[BUS_STATIC_MAX_TRIPS];
  int tripCount = 0;
  StaticStopTime stopTimes[BUS_STATIC_MAX_STOP_TIMES];
  int stopTimeCount = 0;
  StaticServiceDate serviceDates[BUS_STATIC_MAX_SERVICE_DATES];
  int serviceDateCount = 0;
  bool valid = false; // true once at least one successful zip fetch+parse has populated this
};

// Downloads (if missing or older than 24h) and parses GOVA's static
// gtfs.zip, filtered down to only the trips/stop_times/service_dates
// relevant to `stops`/`stopCount` (config.h's BUS_STOPS) -- see CLAUDE.md
// for why filtering DURING the parse (not after) is what makes this fit on
// an ESP32 at all. Held ONLY in RAM/PSRAM for this run's lifetime -- no SD
// card / flash persistence, same "in-memory cache only, resets on reboot"
// design this project already uses for weather (see CLAUDE.md's Storage/
// Reliability notes) -- so a fresh download happens again after every
// reboot, not just every 24h like the Python reference's on-disk cache.
//
// Returns false only when there is NO usable schedule at all (e.g. the
// very first fetch ever attempted failed, or failed partway through
// parsing). A merely-stale-but-already-loaded schedule is left in place
// and this still returns true -- matches gova_next_bus.py's
// ensure_static_zip() "ok to use a stale copy" comment. A schedule that
// fails PARTWAY through parsing (e.g. trips.txt ok but stop_times.txt
// missing) never partially overwrites a previously-good `schedule` --
// it's built up in a separate working copy first and only committed on
// full success.
bool ensureStaticSchedule(StaticSchedule &schedule, const BusStopConfig* stops, int stopCount);

// trip_id -> full StaticTrip lookup within an already-loaded schedule.
// Returns nullptr if not found.
const StaticTrip* findStaticTrip(const StaticSchedule &schedule, const char* tripId);

// One static-schedule candidate for a stop -- always non-live (see
// BusArrival.live in models.h) since this has no realtime backing at all.
struct StaticCandidate {
  char   tripId[24];
  char   route[16];
  char   headsign[32];
  time_t epoch;
};
#define BUS_MAX_STATIC_CANDIDATES 24

// Every static-schedule candidate for one configured stop's routes, across
// yesterday/today/tomorrow relative to nowEpoch -- GTFS's post-midnight
// time format ("24:06:00" etc.) represents a trip using the PREVIOUS
// service day's date even though it's actually happening right now, so
// anchoring to today+tomorrow alone silently misses anything active right
// after local midnight (see clauderef.md section 5 and bus.cpp's merge
// comment for the full reasoning -- this bit the original Python
// implementation once before it was fixed there). Returns the number of
// candidates written (up to maxOut).
int staticCandidatesForStop(const StaticSchedule &schedule, const BusStopConfig &stop,
                             time_t nowEpoch, StaticCandidate* out, int maxOut);

// NOTE on scope vs. the Python reference (gova_next_bus.py): this port
// deliberately does NOT parse stops.txt or carry stop_lat/stop_lon --
// those only ever fed two things there: (a) a stop_name fallback label,
// which this project never needs since BusStopConfig.label (config.h) is
// always provided, and (b) the purely cosmetic "gps_distance_m" figure
// (how far a live-tracked vehicle currently is from the stop), which
// doesn't affect any arrival-time correctness -- stoppedNow (see
// bus_realtime.h) is what actually changes an ETA, and that's kept. Fewer
// moving parts for zero loss of anything that affects what time actually
// shows up on screen; flagged here rather than silently dropped.
