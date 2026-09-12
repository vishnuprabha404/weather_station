#include "bus.h"
#include "bus_static.h"
#include "bus_realtime.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

// The dashboard's configured stops -- config.h's BUS_STOPS_CONFIG is a
// plain macro (pure text, no type dependency of its own; see config.h's
// comment) that only expands into a real array HERE, once BusStopConfig
// (models.h, pulled in via bus.h above) is already visible. Real values
// (which physical stop_ids/routes to show) are gitignored config, not
// hardcoded here -- same reasoning as WEATHER_LATITUDE/LONGITUDE, since
// this repo is public and a stop_id is location-identifying.
static const BusStopConfig BUS_STOPS[BUS_STOP_COUNT] = BUS_STOPS_CONFIG;

// Still show a bus up to this long after its due time (matches
// gova_next_bus.py's GRACE_SECONDS) -- otherwise a bus due "right now"
// flickers out of the list a few seconds before it actually arrives, which
// reads as wrong to someone glancing at the display expecting it to still
// be there.
static const int GRACE_SECONDS = 45;

// One trip+day, after static/realtime merge -- keyed by (tripId, dateKey)
// since a trip_id recurs daily and the two occurrences must not collide
// (see dateKeyForEpoch()/findMergedIndex() below; matches
// gova_next_bus.py's merge_key()).
struct MergedCandidate {
  char   tripId[24];
  char   dateKey[9]; // "YYYYMMDD" of this candidate's LOCAL date
  char   route[16];
  char   headsign[32];
  time_t epoch;
  bool   live;
  int    delayMin;
  bool   hasScheduledTime;
  time_t scheduledEpoch;
};
#define BUS_MAX_MERGED_CANDIDATES 40

static void dateKeyForEpoch(time_t epoch, char* out, size_t outLen) {
  struct tm tmLocal;
  localtime_r(&epoch, &tmLocal);
  strftime(out, outLen, "%Y%m%d", &tmLocal);
}

static int findMergedIndex(const MergedCandidate* arr, int count, const char* tripId, const char* dateKey) {
  for (int i = 0; i < count; i++) {
    if (strcmp(arr[i].tripId, tripId) == 0 && strcmp(arr[i].dateKey, dateKey) == 0) return i;
  }
  return -1;
}

// Does this realtime candidate belong to `stop` (by stop_id) AND to one of
// its watched routes? tmix/Consat (GOVA's vendor) often leaves
// TripDescriptor.route_id blank on the wire, populating only trip_id (see
// bus_realtime.h) -- this falls back to the static trips lookup by tripId
// in that case, same as gova_next_bus.py's realtime_candidates(). Writes
// the resolved route into resolvedRouteOut on a match. Cancelled trips
// (CANCELED/DELETED) never match anything, on purpose -- a cancelled trip
// shouldn't contribute a phantom arrival.
static bool candidateMatchesStop(const RealtimeCandidate &c, const BusStopConfig &stop,
                                  const StaticSchedule &schedule,
                                  char* resolvedRouteOut, size_t resolvedRouteLen) {
  if (strcmp(c.stopId, stop.stopId) != 0) return false;
  if (c.cancelled) return false;

  const char* route = c.route;
  char fallback[16] = "";
  if (route[0] == '\0') {
    const StaticTrip* trip = findStaticTrip(schedule, c.tripId);
    if (trip != nullptr) {
      strncpy(fallback, trip->route, sizeof(fallback) - 1);
      fallback[sizeof(fallback) - 1] = '\0';
      route = fallback;
    }
  }
  if (route[0] == '\0') return false; // no route_id on the wire AND no static fallback -- can't match

  bool routeMatches = false;
  for (int r = 0; r < stop.routeCount; r++) {
    if (strcmp(stop.routes[r], route) == 0) { routeMatches = true; break; }
  }
  if (!routeMatches) return false;

  strncpy(resolvedRouteOut, route, resolvedRouteLen - 1);
  resolvedRouteOut[resolvedRouteLen - 1] = '\0';
  return true;
}

// Builds one stop's BusStopResult: seed from the static schedule, overlay
// realtime (realtime wins for the same trip+day, keeping the static epoch
// around as `scheduledEpoch` rather than silently discarding it -- matches
// gova_next_bus.py's build_stop_result()/merge_key() logic exactly), then
// keep only upcoming-with-grace, sort by epoch, and take the next
// BUS_MAX_ARRIVALS_PER_STOP.
static void buildStopResult(const BusStopConfig &stop, const StaticSchedule &schedule,
                             const RealtimeCandidate* rtCandidates, int rtCount,
                             bool realtimeOk, time_t nowEpoch, BusStopResult &result) {
  strncpy(result.stopId, stop.stopId, sizeof(result.stopId) - 1); result.stopId[sizeof(result.stopId) - 1] = '\0';
  strncpy(result.label, stop.label, sizeof(result.label) - 1);   result.label[sizeof(result.label) - 1] = '\0';
  result.arrivalCount = 0;
  result.realtimeOk = realtimeOk;
  result.hasAnyData = false;

  // ~40 * ~90 bytes ~= 3.6KB -- kept off the stack anyway (static, reused
  // across stops/calls; each call resets mergedCount to 0 before use) for
  // the same "don't trust a task's default stack size" caution the larger
  // structures elsewhere in this module apply more strictly to.
  static MergedCandidate merged[BUS_MAX_MERGED_CANDIDATES];
  int mergedCount = 0;

  // BUG FIX (2026-09-11, found from a real hardware crash-reboot loop --
  // "Stack canary watchpoint triggered", confirmed via objdump: this
  // function's own stack frame was 0x7e0 = 2016 bytes, by far the largest
  // of anything this project has ever put on the stack, entirely because
  // this array was a plain stack local here). BUS_MAX_STATIC_CANDIDATES(24)
  // * sizeof(StaticCandidate) is the exact same class of "too big for a
  // stack frame" case the comment above (on `merged`) already explains --
  // this one just didn't get the same treatment when it was written.
  // `static` here, same reasoning, same pattern.
  static StaticCandidate staticCands[BUS_MAX_STATIC_CANDIDATES];
  int staticCount = staticCandidatesForStop(schedule, stop, nowEpoch, staticCands, BUS_MAX_STATIC_CANDIDATES);
  for (int i = 0; i < staticCount && mergedCount < BUS_MAX_MERGED_CANDIDATES; i++) {
    MergedCandidate &mc = merged[mergedCount++];
    strncpy(mc.tripId, staticCands[i].tripId, sizeof(mc.tripId) - 1);     mc.tripId[sizeof(mc.tripId) - 1] = '\0';
    dateKeyForEpoch(staticCands[i].epoch, mc.dateKey, sizeof(mc.dateKey));
    strncpy(mc.route, staticCands[i].route, sizeof(mc.route) - 1);       mc.route[sizeof(mc.route) - 1] = '\0';
    strncpy(mc.headsign, staticCands[i].headsign, sizeof(mc.headsign) - 1); mc.headsign[sizeof(mc.headsign) - 1] = '\0';
    mc.epoch = staticCands[i].epoch;
    mc.live = false;
    mc.delayMin = 0;
    mc.hasScheduledTime = false;
    mc.scheduledEpoch = 0;
  }

  char resolvedRoute[16];
  for (int i = 0; i < rtCount; i++) {
    if (!candidateMatchesStop(rtCandidates[i], stop, schedule, resolvedRoute, sizeof(resolvedRoute))) continue;

    // A vehicle already STOPPED_AT this exact stop overrides the predicted
    // time with "now" -- see bus_realtime.h's stoppedNow comment.
    time_t epoch = rtCandidates[i].stoppedNow ? nowEpoch : rtCandidates[i].epoch;
    char dateKey[9];
    dateKeyForEpoch(epoch, dateKey, sizeof(dateKey));

    char headsign[32] = ""; // realtime carries no headsign of its own -- always from the static lookup
    const StaticTrip* trip = findStaticTrip(schedule, rtCandidates[i].tripId);
    if (trip != nullptr) {
      strncpy(headsign, trip->headsign, sizeof(headsign) - 1);
      headsign[sizeof(headsign) - 1] = '\0';
    }

    int idx = findMergedIndex(merged, mergedCount, rtCandidates[i].tripId, dateKey);
    if (idx >= 0) {
      // A static counterpart exists for this trip+day -- keep ITS epoch as
      // the scheduled baseline instead of silently overwriting it, so a
      // renderer can show both the original scheduled time and the live
      // delay together (matches gova_next_bus.py's same trade-off).
      MergedCandidate &mc = merged[idx];
      mc.hasScheduledTime = true;
      mc.scheduledEpoch = mc.epoch;
      mc.epoch = epoch;
      mc.live = true;
      mc.delayMin = (int)lround(rtCandidates[i].delaySec / 60.0);
      strncpy(mc.route, resolvedRoute, sizeof(mc.route) - 1); mc.route[sizeof(mc.route) - 1] = '\0';
      if (headsign[0] != '\0') {
        strncpy(mc.headsign, headsign, sizeof(mc.headsign) - 1); mc.headsign[sizeof(mc.headsign) - 1] = '\0';
      }
    } else if (mergedCount < BUS_MAX_MERGED_CANDIDATES) {
      MergedCandidate &mc = merged[mergedCount++];
      strncpy(mc.tripId, rtCandidates[i].tripId, sizeof(mc.tripId) - 1); mc.tripId[sizeof(mc.tripId) - 1] = '\0';
      strncpy(mc.dateKey, dateKey, sizeof(mc.dateKey) - 1);              mc.dateKey[sizeof(mc.dateKey) - 1] = '\0';
      strncpy(mc.route, resolvedRoute, sizeof(mc.route) - 1);            mc.route[sizeof(mc.route) - 1] = '\0';
      strncpy(mc.headsign, headsign, sizeof(mc.headsign) - 1);           mc.headsign[sizeof(mc.headsign) - 1] = '\0';
      mc.epoch = epoch;
      mc.live = true;
      mc.delayMin = (int)lround(rtCandidates[i].delaySec / 60.0);
      mc.hasScheduledTime = false;
      mc.scheduledEpoch = 0;
    }
  }

  // Keep only upcoming (with grace), take the next BUS_MAX_ARRIVALS_PER_STOP
  // by soonest epoch. mergedCount is always small (<= BUS_MAX_MERGED_
  // CANDIDATES), so a plain "pick the smallest not-yet-picked" selection
  // loop is simpler and plenty fast here -- no need for a real sort.
  bool picked[BUS_MAX_MERGED_CANDIDATES] = {false};
  for (int slot = 0; slot < BUS_MAX_ARRIVALS_PER_STOP; slot++) {
    int bestIdx = -1;
    for (int i = 0; i < mergedCount; i++) {
      if (picked[i]) continue;
      if (merged[i].epoch < nowEpoch - GRACE_SECONDS) continue;
      if (bestIdx < 0 || merged[i].epoch < merged[bestIdx].epoch) bestIdx = i;
    }
    if (bestIdx < 0) break;
    picked[bestIdx] = true;

    const MergedCandidate &mc = merged[bestIdx];
    BusArrival &a = result.arrivals[result.arrivalCount++];
    strncpy(a.route, mc.route, sizeof(a.route) - 1);       a.route[sizeof(a.route) - 1] = '\0';
    strncpy(a.headsign, mc.headsign, sizeof(a.headsign) - 1); a.headsign[sizeof(a.headsign) - 1] = '\0';
    // Round to the nearest minute rather than floor -- flooring always
    // under-counts (a bus 4m50s out would read "4 min"), which makes the
    // ETA look consistently a minute short (matches gova_next_bus.py).
    long minutes = lround(difftime(mc.epoch, nowEpoch) / 60.0);
    if (minutes < 0) minutes = 0;
    a.minutes = (int)minutes;
    a.live = mc.live;
    a.delayMin = mc.delayMin;
    a.hasScheduledTime = mc.hasScheduledTime;
    a.epoch = mc.epoch;
    a.scheduledEpoch = mc.scheduledEpoch;
  }

  result.hasAnyData = (staticCount > 0) || (mergedCount > 0);
}

bool fetchAllBuses(BusStopResult* results) {
  // ~42KB -- must be static, never a stack local (see bus_static.h's size
  // comment on StaticSchedule). Persists across calls so ensureStaticSchedule()
  // can skip re-downloading when the in-memory copy is still fresh.
  static StaticSchedule schedule;

  bool staticOk = ensureStaticSchedule(schedule, BUS_STOPS, BUS_STOP_COUNT);

  static RealtimeCandidate rtCandidates[BUS_MAX_REALTIME_CANDIDATES];
  int rtCount = 0;
  bool realtimeOk = fetchRealtimeCandidates(BUS_STOPS, BUS_STOP_COUNT, rtCandidates, &rtCount,
                                             BUS_MAX_REALTIME_CANDIDATES);

  time_t nowEpoch = time(nullptr);

  bool any = false;
  for (int i = 0; i < BUS_STOP_COUNT; i++) {
    buildStopResult(BUS_STOPS[i], schedule, rtCandidates, rtCount, realtimeOk, nowEpoch, results[i]);
    if (results[i].hasAnyData) any = true;
  }

  if (!staticOk && !realtimeOk) {
    Serial.println("[bus] both static schedule and realtime feed unavailable this cycle");
  }

  return any;
}

void formatBusEta(int minutes, bool live, char* out, size_t outLen) {
  if (minutes <= 0) {
    strncpy(out, live ? "now" : "0 min", outLen - 1);
    out[outLen - 1] = '\0';
    return;
  }
  if (minutes < 60) {
    snprintf(out, outLen, "%d min", minutes);
    return;
  }
  int hours = minutes / 60;
  int rem = minutes % 60;
  if (rem == 0) snprintf(out, outLen, "%dh", hours);
  else snprintf(out, outLen, "%dh %dm", hours, rem);
}

void formatBusClock12h(time_t epoch, char* out, size_t outLen) {
  struct tm tmLocal;
  localtime_r(&epoch, &tmLocal);
  strftime(out, outLen, "%I:%M %p", &tmLocal);
}

bool busDisplayChanged(const BusStopResult* a, const BusStopResult* b, int count) {
  for (int i = 0; i < count; i++) {
    const BusStopResult &ra = a[i];
    const BusStopResult &rb = b[i];
    if (ra.arrivalCount != rb.arrivalCount) return true;
    if (ra.hasAnyData != rb.hasAnyData) return true;
    for (int j = 0; j < ra.arrivalCount; j++) {
      const BusArrival &aa = ra.arrivals[j];
      const BusArrival &bb = rb.arrivals[j];
      if (aa.minutes != bb.minutes) return true;
      if (aa.live != bb.live) return true;
      if (aa.delayMin != bb.delayMin) return true;
      if (strcmp(aa.route, bb.route) != 0) return true;
      if (strcmp(aa.headsign, bb.headsign) != 0) return true;
    }
  }
  return false;
}
