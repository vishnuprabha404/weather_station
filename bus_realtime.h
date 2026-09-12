#pragma once
#include "models.h"
#include <time.h>

// One real-time candidate for one configured stop -- see bus_realtime.cpp
// for the GTFS-RT decode strategy (nanopb + gtfs_realtime.pb.h/.c, filtered
// during decode instead of ever materializing a whole city's feed -- see
// CLAUDE.md's "Real-Time Bus Data" section and clauderef.md for why this
// matters on an ESP32).
struct RealtimeCandidate {
  char   stopId[16]; // which stop_id (config.h's BUS_STOPS) this row is for
  char   tripId[24];
  char   route[16];  // AS READ FROM THE WIRE -- may be EMPTY. tmix/Consat (GOVA's vendor) often
                      // leaves TripDescriptor.route_id blank, populating only trip_id; bus.cpp
                      // resolves the fallback via the static trips lookup by tripId, same as
                      // gova_next_bus.py's realtime_candidates() does.
  time_t epoch;       // arrival time if present, else departure (matches gova_next_bus.py)
  int    delaySec;
  bool   cancelled;   // true if TripDescriptor.schedule_relationship was CANCELED or DELETED --
                      // bus.cpp discards these rather than showing a phantom arrival
  bool   stoppedNow;  // true if vehiclepositions.pb has a vehicle on this exact trip currently
                      // STOPPED_AT this exact stop -- bus.cpp treats epoch as "now" when set,
                      // since tripUpdates' predicted time can lag a few minutes behind reality
                      // right as a trip starts (same cross-check gova_next_bus.py does)
};

#define BUS_MAX_REALTIME_CANDIDATES 48

// Fetches tripupdates.pb ONCE and decodes it filtered against the union of
// every stop_id in `stops` (config.h's BUS_STOPS) -- a single HTTP request
// covers every configured stop, not one request per stop (matches
// gova_next_bus.py: one fetch_realtime_feed() call, reused for every
// configured stop). Then fetches vehiclepositions.pb and folds its
// STOPPED_AT signal into stoppedNow for any candidate whose trip has a
// live position report.
//
// Writes up to maxOut candidates into `out` and sets *outCount. Returns
// true if tripupdates.pb was reachable AND fresh (its own FeedHeader.
// timestamp under REALTIME_MAX_AGE_SEC old) -- regardless of whether
// anything in `out` ended up matching our stops (that's a normal "nothing
// due right now" outcome, not a failure). A stale or unreachable feed
// returns false and leaves *outCount at 0 -- caller (bus.cpp) falls back
// to the static schedule alone, same contract as the Python reference.
bool fetchRealtimeCandidates(const BusStopConfig* stops, int stopCount,
                              RealtimeCandidate* out, int* outCount, int maxOut);
