#include "bus_realtime.h"
#include "gtfs_realtime.pb.h"
#include <pb_decode.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// GOVA Transit (Greater Sudbury) is served by Consat/tmix -- these are the
// SAME two endpoints as bus_static.cpp's static gtfs.zip, just the
// real-time half. No auth needed (a quirk of this specific small-city
// agency publishing fully open feeds -- see clauderef.md; don't assume the
// same holds if this project's target agency ever changes). Not in
// config.h: unlike WEATHER_EC_SITE_ID etc., these URLs are the same for
// every GOVA user, not something specific to this deployment -- matches
// how gova_next_bus.py treats them as plain module constants, not config.
static const char* RT_TRIPUPDATES_URL      = "https://sudbury.tmix.se/gtfs-realtime/tripupdates.pb";
static const char* RT_VEHICLEPOSITIONS_URL = "https://sudbury.tmix.se/gtfs-realtime/vehiclepositions.pb";

static const uint32_t HTTP_TIMEOUT_MS = 10000;

// If the feed is reachable but its own header timestamp is older than this,
// treat it as stale rather than live -- a frozen/stuck feed on the agency's
// end would otherwise look identical to a healthy one. Matches
// gova_next_bus.py's REALTIME_MAX_AGE (10 minutes, the same threshold
// Transit-app-style consumers commonly use for agency feed staleness).
static const double REALTIME_MAX_AGE_SEC = 600.0;

// Small scratch state the decodeStopTimeUpdate() callback below closes
// over -- nanopb decode callbacks are plain C function pointers (only a
// void* arg to carry context), so the alternative to a couple of file-scope
// statics here would be threading a context struct through pb_callback_t.arg
// for every one of the (up to 200) pre-armed callback slots; a handful of
// small globals reset at the top of each fetchRealtimeCandidates() call is
// simpler and reads more like this project's other files (see the
// "Coding Principles" section of CLAUDE.md: prefer simple over abstracted).

// Which physical stop_ids this fetch cares about (the union across every
// configured BusStopConfig -- two configs CAN legitimately share a
// stop_id). 8 is generous headroom over any realistic number of physical
// stops one dashboard would show.
#define BUS_REALTIME_MAX_WATCHED_STOPS 8
static char g_watchedStopIds[BUS_REALTIME_MAX_WATCHED_STOPS][16];
static int  g_watchedStopIdCount = 0;

// One matched stop_time_update row, captured DURING decode. Deliberately
// does NOT resolve trip_id/route_id here -- see decodeStopTimeUpdate()'s
// comment below for why that has to wait until after the whole
// pb_decode() call returns.
struct RawStopTimeMatch {
  int16_t entityIndex;
  char    stopId[16];
  time_t  epoch;
  int     delaySec;
};
static RawStopTimeMatch g_rawMatches[BUS_MAX_REALTIME_CANDIDATES];
static int g_rawMatchCount = 0;
static int g_rawMatchOverflowCount = 0; // logged once after decode, not per-row

// Fetches a whole HTTPS response body into a PSRAM-backed heap buffer.
// Binary content (protobuf), so this reads raw bytes off the stream itself
// rather than using HTTPClient::getString() (built for text/String use --
// see weather_ec.cpp/weather_swob.cpp for that pattern; this file needs its
// own because the payload here is binary, not JSON). Caller owns *outBuf
// and must free() it. Same WiFiClientSecure + setInsecure() quick-start
// trade-off as every other HTTPS call in this project -- see weather_ec.cpp's
// comment for the reasoning; same open item to revisit if this ever needs
// to be hardened (see CLAUDE.md's "Real-Time Bus Data" HTTPS/TLS note).
static bool fetchBinary(const char* url, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[bus_realtime] skip fetch: WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("[bus_realtime] http.begin() failed for %s\n", url);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[bus_realtime] GET %s failed, code=%d\n", url, httpCode);
    http.end();
    return false;
  }

  // tmix has always sent a real Content-Length for these two feeds in
  // practice -- if that's ever NOT true (e.g. chunked transfer-encoding),
  // this fetch fails cleanly rather than guessing a buffer size, which is
  // the right trade-off given this can't be verified against the live feed
  // from here (no hardware/network access at write time -- confirm this
  // works against the real endpoint on first hardware test).
  int len = http.getSize();
  if (len <= 0) {
    Serial.printf("[bus_realtime] %s: no usable Content-Length (got %d)\n", url, len);
    http.end();
    return false;
  }

  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.println("[bus_realtime] out of (PSRAM) memory for response body");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long start = millis();
  while (got < (size_t)len && (millis() - start) < HTTP_TIMEOUT_MS) {
    if (stream->available()) {
      int n = stream->read(buf + got, (size_t)len - got);
      if (n > 0) got += (size_t)n;
    } else {
      delay(1); // yield while waiting for more bytes rather than a tight spin
    }
  }
  http.end();

  if (got != (size_t)len) {
    Serial.printf("[bus_realtime] %s: short read, got %u of %d bytes\n", url, (unsigned)got, len);
    free(buf);
    return false;
  }

  *outBuf = buf;
  *outLen = got;
  return true;
}

// nanopb decode callback for TripUpdate.stop_time_update (a FT_CALLBACK
// field -- see proto_src/gtfs-realtime-trimmed.options for why: a trip can
// have dozens of these, and a whole-city feed can have many trips, so this
// filters/discards row-by-row during decode instead of ever materializing
// a full array -- the one field in this feed that's genuinely unbounded).
//
// Deliberately does NOT resolve which trip_id/route_id this row belongs to
// -- that lives in the SIBLING field TripUpdate.trip (tag 1), which is
// only guaranteed to already be decoded at this point if the producer
// serialized fields in ascending tag order. That's the near-universal
// convention (and what every mainstream protobuf encoder does), but isn't
// guaranteed by the protobuf spec itself, so this callback only records
// {entityIndex, stopId, epoch, delaySec} and fetchRealtimeCandidates()
// resolves trip/route in a second pass AFTER the whole pb_decode() call
// returns, once entity[k].trip_update.trip is unconditionally fully
// populated for every k. Slightly more bookkeeping, zero assumptions.
static bool decodeStopTimeUpdate(pb_istream_t *stream, const pb_field_t *field, void **arg) {
  (void)field;
  transit_realtime_TripUpdate_StopTimeUpdate stu = transit_realtime_TripUpdate_StopTimeUpdate_init_zero;
  if (!pb_decode(stream, transit_realtime_TripUpdate_StopTimeUpdate_fields, &stu)) {
    return false; // a genuinely malformed row -- let the whole decode fail loudly rather than skip it
  }

  // SKIPPED means this trip is bypassing the stop today (e.g. a detour) --
  // it still carries a (meaningless) arrival/departure time, so without
  // this check a bus that isn't actually coming here would show up with a
  // normal-looking countdown. Matches gova_next_bus.py's same check.
  if (stu.has_schedule_relationship &&
      stu.schedule_relationship ==
          transit_realtime_TripUpdate_StopTimeUpdate_ScheduleRelationship_SKIPPED) {
    return true;
  }
  if (!stu.has_stop_id) return true;

  bool watched = false;
  for (int i = 0; i < g_watchedStopIdCount; i++) {
    if (strcmp(stu.stop_id, g_watchedStopIds[i]) == 0) { watched = true; break; }
  }
  if (!watched) return true; // not one of our stops -- the common case for most of a city's feed

  const transit_realtime_TripUpdate_StopTimeEvent* ev = nullptr;
  if (stu.has_arrival) ev = &stu.arrival;
  else if (stu.has_departure) ev = &stu.departure;
  if (ev == nullptr || !ev->has_time || ev->time == 0) return true;

  if (g_rawMatchCount >= BUS_MAX_REALTIME_CANDIDATES) {
    g_rawMatchOverflowCount++;
    return true; // drop silently here; a summary warning is logged once after decode completes
  }

  RawStopTimeMatch &m = g_rawMatches[g_rawMatchCount++];
  m.entityIndex = (int16_t)(intptr_t)(*arg); // stashed as a plain index, not a real pointer -- see
                                              // the pre-decode setup loop in fetchRealtimeCandidates()
  strncpy(m.stopId, stu.stop_id, sizeof(m.stopId) - 1);
  m.stopId[sizeof(m.stopId) - 1] = '\0';
  m.epoch = (time_t)ev->time;
  m.delaySec = ev->has_delay ? ev->delay : 0;
  return true;
}

bool fetchRealtimeCandidates(const BusStopConfig* stops, int stopCount,
                              RealtimeCandidate* out, int* outCount, int maxOut) {
  *outCount = 0;
  g_rawMatchCount = 0;
  g_rawMatchOverflowCount = 0;

  g_watchedStopIdCount = 0;
  for (int i = 0; i < stopCount && g_watchedStopIdCount < BUS_REALTIME_MAX_WATCHED_STOPS; i++) {
    bool already = false; // two configured entries CAN legitimately share a physical stop_id
    for (int j = 0; j < g_watchedStopIdCount; j++) {
      if (strcmp(g_watchedStopIds[j], stops[i].stopId) == 0) { already = true; break; }
    }
    if (already) continue;
    strncpy(g_watchedStopIds[g_watchedStopIdCount], stops[i].stopId, sizeof(g_watchedStopIds[0]) - 1);
    g_watchedStopIds[g_watchedStopIdCount][sizeof(g_watchedStopIds[0]) - 1] = '\0';
    g_watchedStopIdCount++;
  }
  if (stopCount > BUS_REALTIME_MAX_WATCHED_STOPS) {
    Serial.printf("[bus_realtime] WARNING: %d configured stops but only watching the first %d "
                  "distinct stop_ids -- raise BUS_REALTIME_MAX_WATCHED_STOPS if this is real\n",
                  stopCount, BUS_REALTIME_MAX_WATCHED_STOPS);
  }

  uint8_t* raw = nullptr;
  size_t rawLen = 0;
  if (!fetchBinary(RT_TRIPUPDATES_URL, &raw, &rawLen)) {
    return false;
  }

  transit_realtime_FeedMessage* msg =
      (transit_realtime_FeedMessage*)heap_caps_malloc(sizeof(transit_realtime_FeedMessage), MALLOC_CAP_SPIRAM);
  if (!msg) {
    Serial.println("[bus_realtime] out of PSRAM for FeedMessage (tripupdates)");
    free(raw);
    return false;
  }
  memset(msg, 0, sizeof(transit_realtime_FeedMessage));

  // Arm the nested stop_time_update callback on EVERY entity slot before
  // decoding starts. nanopb decodes a fixed-count repeated submessage field
  // element-by-element directly into the array as it encounters each
  // occurrence on the wire, so whichever slot ends up holding entity[k]
  // needs its callback already configured -- we don't know k in advance,
  // hence every slot gets set up identically. This is exactly why
  // stop_time_update is the ONE field marked FT_CALLBACK (see
  // proto_src/gtfs-realtime-trimmed.options) while entity itself stays a
  // plain fixed array: only the genuinely unbounded field needs this.
  const int feedMaxEntities = (int)(sizeof(msg->entity) / sizeof(msg->entity[0]));
  for (int i = 0; i < feedMaxEntities; i++) {
    msg->entity[i].trip_update.stop_time_update.funcs.decode = &decodeStopTimeUpdate;
    msg->entity[i].trip_update.stop_time_update.arg = (void*)(intptr_t)i;
  }

  pb_istream_t stream = pb_istream_from_buffer(raw, rawLen);
  bool decodeOk = pb_decode(&stream, transit_realtime_FeedMessage_fields, msg);
  free(raw); // fully decoded into msg now; the raw bytes aren't needed again

  if (!decodeOk) {
    Serial.printf("[bus_realtime] tripupdates.pb decode failed: %s\n", PB_GET_ERROR(&stream));
    free(msg);
    return false;
  }

  if (g_rawMatchOverflowCount > 0) {
    Serial.printf("[bus_realtime] WARNING: %d matching stop_time_update rows dropped -- "
                  "BUS_MAX_REALTIME_CANDIDATES (%d) too small for this feed\n",
                  g_rawMatchOverflowCount, BUS_MAX_REALTIME_CANDIDATES);
  }

  bool fresh = true;
  if (msg->has_header && msg->header.has_timestamp && msg->header.timestamp > 0) {
    double ageSec = difftime(time(nullptr), (time_t)msg->header.timestamp);
    if (ageSec > REALTIME_MAX_AGE_SEC) {
      Serial.printf("[bus_realtime] tripupdates.pb is stale (%.0fs old); ignoring, "
                    "falling back to static schedule\n", ageSec);
      fresh = false;
    }
  } else {
    Serial.println("[bus_realtime] tripupdates.pb has no header timestamp -- "
                    "treating as fresh (nothing to compare against)");
  }

  if (!fresh) {
    free(msg);
    return false;
  }

  // Second pass: resolve each raw match's real trip_id/route_id/
  // cancellation state now that decode is fully complete (see
  // decodeStopTimeUpdate()'s comment for why this waits until here).
  int n = 0;
  for (int i = 0; i < g_rawMatchCount && n < maxOut; i++) {
    const RawStopTimeMatch &m = g_rawMatches[i];
    if (m.entityIndex < 0 || m.entityIndex >= feedMaxEntities) continue; // defensive; shouldn't happen
    const transit_realtime_FeedEntity &ent = msg->entity[m.entityIndex];
    if (!ent.has_trip_update || !ent.trip_update.has_trip) continue;
    const transit_realtime_TripDescriptor &trip = ent.trip_update.trip;

    RealtimeCandidate &c = out[n++];
    memset(&c, 0, sizeof(c));
    strncpy(c.stopId, m.stopId, sizeof(c.stopId) - 1);
    if (trip.has_trip_id)  strncpy(c.tripId, trip.trip_id, sizeof(c.tripId) - 1);
    if (trip.has_route_id) strncpy(c.route, trip.route_id, sizeof(c.route) - 1);
    c.epoch = m.epoch;
    c.delaySec = m.delaySec;
    c.cancelled = trip.has_schedule_relationship &&
        (trip.schedule_relationship == transit_realtime_TripDescriptor_ScheduleRelationship_CANCELED ||
         trip.schedule_relationship == transit_realtime_TripDescriptor_ScheduleRelationship_DELETED);
    c.stoppedNow = false; // possibly set true below, by the vehiclepositions.pb pass
  }
  *outCount = n;
  free(msg);

  // --- vehiclepositions.pb: second, independent live signal --------------
  // A vehicle already STOPPED_AT the exact stop we're tracking overrides
  // the predicted time from tripUpdates, which can lag a few minutes
  // behind reality right as a trip starts -- same cross-check
  // gova_next_bus.py's fetch_vehicle_positions_feed()/realtime_candidates()
  // do. Best-effort only: unlike tripupdates.pb above, a failure here
  // doesn't change this function's return value -- the candidates already
  // collected are perfectly usable on their own, just without the "the bus
  // is here right now" override.
  uint8_t* vpRaw = nullptr;
  size_t vpLen = 0;
  if (fetchBinary(RT_VEHICLEPOSITIONS_URL, &vpRaw, &vpLen)) {
    transit_realtime_FeedMessage* vpMsg =
        (transit_realtime_FeedMessage*)heap_caps_malloc(sizeof(transit_realtime_FeedMessage), MALLOC_CAP_SPIRAM);
    if (vpMsg) {
      memset(vpMsg, 0, sizeof(transit_realtime_FeedMessage));
      // No nested callback needed here: VehiclePosition has no large
      // repeated field, so every entity slot decodes fully "static" --
      // stop_time_update's callback slots stay unarmed (funcs.decode is
      // NULL from the memset above), which nanopb treats as "skip this
      // field" -- harmless, since vehicle-position entities never have a
      // trip_update to begin with.
      pb_istream_t vpStream = pb_istream_from_buffer(vpRaw, vpLen);
      if (pb_decode(&vpStream, transit_realtime_FeedMessage_fields, vpMsg)) {
        bool vpFresh = true;
        if (vpMsg->has_header && vpMsg->header.has_timestamp && vpMsg->header.timestamp > 0) {
          double ageSec = difftime(time(nullptr), (time_t)vpMsg->header.timestamp);
          if (ageSec > REALTIME_MAX_AGE_SEC) vpFresh = false;
        }
        if (vpFresh) {
          for (int i = 0; i < vpMsg->entity_count; i++) {
            const transit_realtime_FeedEntity &ent = vpMsg->entity[i];
            if (!ent.has_vehicle) continue;
            const transit_realtime_VehiclePosition &vp = ent.vehicle;
            if (!vp.has_trip || !vp.trip.has_trip_id || !vp.has_stop_id) continue;
            if (!vp.has_current_status ||
                vp.current_status != transit_realtime_VehiclePosition_VehicleStopStatus_STOPPED_AT) {
              continue;
            }
            for (int k = 0; k < n; k++) {
              if (strcmp(out[k].tripId, vp.trip.trip_id) == 0 && strcmp(out[k].stopId, vp.stop_id) == 0) {
                out[k].stoppedNow = true;
              }
            }
          }
        } else {
          Serial.println("[bus_realtime] vehiclepositions.pb is stale; ignoring");
        }
      } else {
        Serial.printf("[bus_realtime] vehiclepositions.pb decode failed: %s\n", PB_GET_ERROR(&vpStream));
      }
      free(vpMsg);
    }
    free(vpRaw);
  }
  // A missing/failed vehiclepositions.pb fetch is NOT logged as a hard
  // failure here -- tripupdates.pb alone (already returned true above) is
  // a perfectly usable result without it.

  return true;
}
