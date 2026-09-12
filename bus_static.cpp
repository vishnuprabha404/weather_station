#include "bus_static.h"
#include <miniz.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// GOVA Transit (Greater Sudbury) via Consat/tmix -- SAME vendor/agency as
// bus_realtime.cpp's two endpoints, just the static half. No auth needed
// (see that file's comment -- a quirk of this specific small-city agency,
// don't assume it elsewhere). Not in config.h for the same reason as
// bus_realtime.cpp's URLs: this is the same for every GOVA user, not
// specific to this deployment.
static const char* STATIC_GTFS_URL = "https://sudbury.tmix.se/gtfs/gtfs.zip";

static const uint32_t HTTP_TIMEOUT_MS = 15000; // gtfs.zip is bigger than the realtime feeds; more headroom

// Matches gova_next_bus.py's STATIC_MAX_AGE (24h) -- the static schedule
// rarely changes more often than that. Unlike the Python reference (which
// caches gtfs.zip on disk across separate process runs), this project has
// no persistent storage in play anywhere (see CLAUDE.md's Storage notes),
// so this age check only ever matters WITHIN one power-on session; a
// reboot always re-downloads regardless of how recently it last ran.
static const unsigned long STATIC_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL;
static unsigned long g_lastFetchMs = 0;

#define CSV_MAX_COLUMNS   24
#define CSV_LINE_BUF_SIZE 512
static char g_csvLineBuf[CSV_LINE_BUF_SIZE];

// BUG FIX (2026-09-11, found from a real hardware crash -- see CLAUDE.md's
// "Known Bugs Fixed"): miniz's mz_zip_archive defaults m_pAlloc/m_pFree/
// m_pRealloc to plain malloc()/free()/realloc() (this chip's INTERNAL SRAM,
// a few hundred KB total) for EVERY allocation it makes whenever they're
// left NULL/zero -- confirmed by reading mz_zip_reader_init_internal() and
// mz_zip_reader_extract_to_heap() directly, not assumed. That's fatal here:
// GOVA's real stop_times.txt decompresses to ~4.78MB and calendar_dates.txt
// to ~1.9MB (confirmed by actually downloading and unzipping the real
// feed -- these numbers are real, not estimated), both wildly larger than
// internal SRAM. The failed/degenerate allocation corrupted the heap
// badly enough to crash later in a COMPLETELY unrelated place (the WiFi
// driver's own packet-receive path, mid-allocation) -- a classic "crash
// site is the victim, not the cause" heap-corruption signature. These
// three callbacks route every miniz allocation through PSRAM instead,
// matching this project's own convention for anything this size (see
// display.cpp's framebuffer, and this file's other large PSRAM buffers).
static void* zipPsramAlloc(void* opaque, size_t items, size_t size) {
  (void)opaque;
  return heap_caps_malloc(items * size, MALLOC_CAP_SPIRAM);
}
static void zipPsramFree(void* opaque, void* address) {
  (void)opaque;
  heap_caps_free(address);
}
static void* zipPsramRealloc(void* opaque, void* address, size_t items, size_t size) {
  (void)opaque;
  return heap_caps_realloc(address, items * size, MALLOC_CAP_SPIRAM);
}

// --- shared HTTP + CSV plumbing --------------------------------------------

// Same binary-fetch shape as bus_realtime.cpp's fetchBinary() -- duplicated
// rather than shared between the two files, same "avoid unnecessary
// abstraction for a single-purpose embedded firmware" call this project
// already made for weather_ec.cpp/weather_swob.cpp's near-identical
// timestamp-conversion helper (see that file's comment). Caller owns
// *outBuf and must free() it.
static bool fetchBinary(const char* url, uint8_t** outBuf, size_t* outLen) {
  *outBuf = nullptr;
  *outLen = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[bus_static] skip fetch: WiFi not connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // same quick-start trade-off as every other HTTPS call in this project

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.printf("[bus_static] http.begin() failed for %s\n", url);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[bus_static] GET %s failed, code=%d\n", url, httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) {
    Serial.printf("[bus_static] %s: no usable Content-Length (got %d)\n", url, len);
    http.end();
    return false;
  }

  uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.println("[bus_static] out of (PSRAM) memory for response body");
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
      delay(1);
    }
  }
  http.end();

  if (got != (size_t)len) {
    Serial.printf("[bus_static] %s: short read, got %u of %d bytes\n", url, (unsigned)got, len);
    free(buf);
    return false;
  }

  *outBuf = buf;
  *outLen = got;
  return true;
}

// GOVA's CSVs are UTF-8-with-BOM -- bit the Kotlin port once, silently
// (see CLAUDE.md/clauderef.md: the CSV reader there didn't strip it and
// every row lookup just quietly failed, no exception thrown). Adjusts
// *buf/*len past a leading EF BB BF if present; a no-op otherwise.
static void stripBom(const uint8_t** buf, size_t* len) {
  if (*len >= 3 && (*buf)[0] == 0xEF && (*buf)[1] == 0xBB && (*buf)[2] == 0xBF) {
    *buf += 3;
    *len -= 3;
  }
}

// Finds the next line within [buf, buf+len) starting at *pos (buf need NOT
// be NUL-terminated -- miniz's extract_to_heap() returns exactly the
// decompressed byte count with no added terminator). Returns a pointer to
// the line's first byte and sets *lineLen (EXCLUDING the \r?\n/\n/\r
// terminator), and advances *pos past that terminator. Returns nullptr
// once there are no more lines.
static const char* nextCsvLine(const char* buf, size_t len, size_t* pos, size_t* lineLen) {
  if (*pos >= len) return nullptr;
  size_t start = *pos;
  size_t i = start;
  while (i < len && buf[i] != '\n' && buf[i] != '\r') i++;
  *lineLen = i - start;
  if (i < len && buf[i] == '\r') i++;
  if (i < len && buf[i] == '\n') i++;
  *pos = i;
  return buf + start;
}

// Copies one (buf,len) line into the shared NUL-terminated scratch buffer
// so the field splitter below can work with a normal C string. Shared/
// reused across calls (never two lines "in flight" at once -- each line is
// fully parsed and its needed fields copied OUT into a StaticTrip/
// StaticStopTime/StaticServiceDate before the next line is read), same
// "process one row, keep only what matters, move on" approach
// bus_realtime.cpp's decodeStopTimeUpdate() uses.
static char* copyLineToScratch(const char* line, size_t lineLen) {
  if (lineLen >= CSV_LINE_BUF_SIZE) {
    Serial.printf("[bus_static] WARNING: CSV line too long (%u bytes), truncating\n", (unsigned)lineLen);
    lineLen = CSV_LINE_BUF_SIZE - 1;
  }
  memcpy(g_csvLineBuf, line, lineLen);
  g_csvLineBuf[lineLen] = '\0';
  return g_csvLineBuf;
}

// Extracts one CSV field starting at *cursor and advances *cursor past it
// (RFC 4180: a double-quoted field's comma isn't a separator, and ""
// inside a quoted field collapses to a literal "). NUL-terminates the
// field IN PLACE inside the same buffer (no separate allocation) and
// returns a pointer to its start. Safe to call repeatedly past the actual
// end of the row's real columns -- once *cursor reaches the line's own
// terminating NUL it just keeps returning an empty string forever rather
// than reading past the buffer, which is what lets parseCsvRow() below pad
// a short/malformed row instead of crashing.
static char* nextCsvField(char** cursor) {
  char* p = *cursor;
  char* fieldStart;

  if (*p == '"') {
    p++; // skip opening quote
    fieldStart = p;
    char* w = p;
    while (*p != '\0') {
      if (*p == '"') {
        if (*(p + 1) == '"') { *w++ = '"'; p += 2; continue; }
        p++; // real closing quote
        break;
      }
      *w++ = *p++;
    }
    *w = '\0';
    while (*p != '\0' && *p != ',') p++; // tolerate stray bytes between the closing quote and the comma
  } else {
    fieldStart = p;
    while (*p != '\0' && *p != ',') p++;
  }

  if (*p == ',') { *p = '\0'; *cursor = p + 1; }
  else { *cursor = p; } // already at the line's terminating NUL -- stay put

  return fieldStart;
}

// Splits one (mutable, NUL-terminated) CSV line into up to maxCols fields.
static int parseCsvRow(char* line, char** fields, int maxCols) {
  char* cursor = line;
  int n = 0;
  while (n < maxCols) {
    fields[n++] = nextCsvField(&cursor);
  }
  return n;
}

static int findColumn(char** headerFields, int headerCount, const char* columnName) {
  for (int i = 0; i < headerCount; i++) {
    if (strcmp(headerFields[i], columnName) == 0) return i;
  }
  return -1;
}

// "HH:MM:SS" -> seconds since midnight. HH may be >= 24 for post-midnight
// service (GTFS's own convention for representing a trip that runs into
// the next calendar day without changing its service date) -- this is
// exactly why staticCandidatesForStop() has to check yesterday/today/
// tomorrow rather than just today/tomorrow. Returns -1 for anything that
// doesn't parse as three colon-separated integers.
static int parseGtfsTime(const char* hms) {
  int h = 0, m = 0, s = 0;
  if (sscanf(hms, "%d:%d:%d", &h, &m, &s) != 3) return -1;
  return h * 3600 + m * 60 + s;
}

static bool routeIsWatched(const BusStopConfig* stops, int stopCount, const char* routeId) {
  for (int i = 0; i < stopCount; i++) {
    for (int j = 0; j < stops[i].routeCount; j++) {
      if (strcmp(stops[i].routes[j], routeId) == 0) return true;
    }
  }
  return false;
}

static bool stopIdIsWatched(const BusStopConfig* stops, int stopCount, const char* stopId) {
  for (int i = 0; i < stopCount; i++) {
    if (strcmp(stops[i].stopId, stopId) == 0) return true;
  }
  return false;
}

// --- per-file parsers -------------------------------------------------------
// Order matters: trips.txt must be parsed FIRST (stop_times.txt and
// calendar_dates.txt both filter against the trip set it produces), same
// order gova_next_bus.py's load_static_data() uses.

static bool parseTripsCsv(const uint8_t* buf, size_t len, const BusStopConfig* stops, int stopCount,
                           StaticSchedule &schedule) {
  size_t pos = 0, lineLen = 0;
  const char* line = nextCsvLine((const char*)buf, len, &pos, &lineLen);
  if (line == nullptr) { Serial.println("[bus_static] trips.txt: empty file"); return false; }

  char* header = copyLineToScratch(line, lineLen);
  char* headerFields[CSV_MAX_COLUMNS];
  int headerCount = parseCsvRow(header, headerFields, CSV_MAX_COLUMNS);
  int idxRouteId   = findColumn(headerFields, headerCount, "route_id");
  int idxTripId    = findColumn(headerFields, headerCount, "trip_id");
  int idxServiceId = findColumn(headerFields, headerCount, "service_id");
  int idxHeadsign  = findColumn(headerFields, headerCount, "trip_headsign"); // optional; -1 is fine

  if (idxRouteId < 0 || idxTripId < 0 || idxServiceId < 0) {
    Serial.println("[bus_static] trips.txt: missing a required column (route_id/trip_id/service_id)");
    return false;
  }

  while ((line = nextCsvLine((const char*)buf, len, &pos, &lineLen)) != nullptr) {
    if (lineLen == 0) continue; // blank trailing line
    char* row = copyLineToScratch(line, lineLen);
    char* fields[CSV_MAX_COLUMNS];
    int n = parseCsvRow(row, fields, CSV_MAX_COLUMNS);
    if (idxRouteId >= n || idxTripId >= n || idxServiceId >= n) continue; // short/malformed row

    if (!routeIsWatched(stops, stopCount, fields[idxRouteId])) continue;

    if (schedule.tripCount >= BUS_STATIC_MAX_TRIPS) {
      Serial.println("[bus_static] WARNING: BUS_STATIC_MAX_TRIPS reached, some trips.txt rows dropped");
      break;
    }
    StaticTrip &t = schedule.trips[schedule.tripCount++];
    strncpy(t.tripId, fields[idxTripId], sizeof(t.tripId) - 1);       t.tripId[sizeof(t.tripId) - 1] = '\0';
    strncpy(t.route, fields[idxRouteId], sizeof(t.route) - 1);        t.route[sizeof(t.route) - 1] = '\0';
    strncpy(t.serviceId, fields[idxServiceId], sizeof(t.serviceId) - 1); t.serviceId[sizeof(t.serviceId) - 1] = '\0';
    if (idxHeadsign >= 0 && idxHeadsign < n) {
      strncpy(t.headsign, fields[idxHeadsign], sizeof(t.headsign) - 1);
      t.headsign[sizeof(t.headsign) - 1] = '\0';
    } else {
      t.headsign[0] = '\0';
    }
  }
  Serial.printf("[bus_static] trips.txt: kept %d matching trips\n", schedule.tripCount);
  return true;
}

static bool parseStopTimesCsv(const uint8_t* buf, size_t len, const BusStopConfig* stops, int stopCount,
                               StaticSchedule &schedule) {
  size_t pos = 0, lineLen = 0;
  const char* line = nextCsvLine((const char*)buf, len, &pos, &lineLen);
  if (line == nullptr) { Serial.println("[bus_static] stop_times.txt: empty file"); return false; }

  char* header = copyLineToScratch(line, lineLen);
  char* headerFields[CSV_MAX_COLUMNS];
  int headerCount = parseCsvRow(header, headerFields, CSV_MAX_COLUMNS);
  int idxTripId    = findColumn(headerFields, headerCount, "trip_id");
  int idxStopId    = findColumn(headerFields, headerCount, "stop_id");
  int idxArrival   = findColumn(headerFields, headerCount, "arrival_time");
  int idxDeparture = findColumn(headerFields, headerCount, "departure_time");

  if (idxTripId < 0 || idxStopId < 0 || (idxArrival < 0 && idxDeparture < 0)) {
    Serial.println("[bus_static] stop_times.txt: missing a required column");
    return false;
  }

  while ((line = nextCsvLine((const char*)buf, len, &pos, &lineLen)) != nullptr) {
    if (lineLen == 0) continue;
    char* row = copyLineToScratch(line, lineLen);
    char* fields[CSV_MAX_COLUMNS];
    int n = parseCsvRow(row, fields, CSV_MAX_COLUMNS);
    if (idxTripId >= n || idxStopId >= n) continue;

    if (!stopIdIsWatched(stops, stopCount, fields[idxStopId])) continue;
    if (findStaticTrip(schedule, fields[idxTripId]) == nullptr) continue; // not one of our watched trips

    const char* timeStr = "";
    if (idxArrival >= 0 && idxArrival < n && fields[idxArrival][0] != '\0') {
      timeStr = fields[idxArrival];
    } else if (idxDeparture >= 0 && idxDeparture < n) {
      timeStr = fields[idxDeparture]; // matches gova_next_bus.py: row["arrival_time"] or row["departure_time"]
    }
    int seconds = parseGtfsTime(timeStr);
    if (seconds < 0) continue; // malformed/missing time -- skip rather than guess

    if (schedule.stopTimeCount >= BUS_STATIC_MAX_STOP_TIMES) {
      Serial.println("[bus_static] WARNING: BUS_STATIC_MAX_STOP_TIMES reached, some stop_times.txt rows dropped");
      break;
    }
    StaticStopTime &st = schedule.stopTimes[schedule.stopTimeCount++];
    strncpy(st.tripId, fields[idxTripId], sizeof(st.tripId) - 1); st.tripId[sizeof(st.tripId) - 1] = '\0';
    strncpy(st.stopId, fields[idxStopId], sizeof(st.stopId) - 1); st.stopId[sizeof(st.stopId) - 1] = '\0';
    st.secondsSinceMidnight = seconds;
  }
  Serial.printf("[bus_static] stop_times.txt: kept %d matching rows\n", schedule.stopTimeCount);
  return true;
}

static bool parseCalendarDatesCsv(const uint8_t* buf, size_t len, StaticSchedule &schedule) {
  size_t pos = 0, lineLen = 0;
  const char* line = nextCsvLine((const char*)buf, len, &pos, &lineLen);
  if (line == nullptr) { Serial.println("[bus_static] calendar_dates.txt: empty file"); return false; }

  char* header = copyLineToScratch(line, lineLen);
  char* headerFields[CSV_MAX_COLUMNS];
  int headerCount = parseCsvRow(header, headerFields, CSV_MAX_COLUMNS);
  int idxServiceId     = findColumn(headerFields, headerCount, "service_id");
  int idxDate           = findColumn(headerFields, headerCount, "date");
  int idxExceptionType  = findColumn(headerFields, headerCount, "exception_type");

  if (idxServiceId < 0 || idxDate < 0 || idxExceptionType < 0) {
    Serial.println("[bus_static] calendar_dates.txt: missing a required column");
    return false;
  }

  while ((line = nextCsvLine((const char*)buf, len, &pos, &lineLen)) != nullptr) {
    if (lineLen == 0) continue;
    char* row = copyLineToScratch(line, lineLen);
    char* fields[CSV_MAX_COLUMNS];
    int n = parseCsvRow(row, fields, CSV_MAX_COLUMNS);
    if (idxServiceId >= n || idxDate >= n || idxExceptionType >= n) continue;

    if (strcmp(fields[idxExceptionType], "1") != 0) continue; // only ADDED dates -- see struct comment

    bool relevant = false; // only keep dates for service_ids our watched trips actually use
    for (int i = 0; i < schedule.tripCount; i++) {
      if (strcmp(schedule.trips[i].serviceId, fields[idxServiceId]) == 0) { relevant = true; break; }
    }
    if (!relevant) continue;

    if (schedule.serviceDateCount >= BUS_STATIC_MAX_SERVICE_DATES) {
      Serial.println("[bus_static] WARNING: BUS_STATIC_MAX_SERVICE_DATES reached, some rows dropped");
      break;
    }
    StaticServiceDate &sd = schedule.serviceDates[schedule.serviceDateCount++];
    strncpy(sd.serviceId, fields[idxServiceId], sizeof(sd.serviceId) - 1); sd.serviceId[sizeof(sd.serviceId) - 1] = '\0';
    strncpy(sd.date, fields[idxDate], sizeof(sd.date) - 1);                sd.date[sizeof(sd.date) - 1] = '\0';
  }
  Serial.printf("[bus_static] calendar_dates.txt: kept %d matching service dates\n", schedule.serviceDateCount);
  return true;
}

// --- public API --------------------------------------------------------

const StaticTrip* findStaticTrip(const StaticSchedule &schedule, const char* tripId) {
  for (int i = 0; i < schedule.tripCount; i++) {
    if (strcmp(schedule.trips[i].tripId, tripId) == 0) return &schedule.trips[i];
  }
  return nullptr;
}

bool ensureStaticSchedule(StaticSchedule &schedule, const BusStopConfig* stops, int stopCount) {
  unsigned long now = millis();
  if (schedule.valid && (now - g_lastFetchMs) < STATIC_MAX_AGE_MS) {
    return true; // already loaded and fresh enough
  }

  uint8_t* zipBuf = nullptr;
  size_t zipLen = 0;
  if (!fetchBinary(STATIC_GTFS_URL, &zipBuf, &zipLen)) {
    if (schedule.valid) {
      Serial.println("[bus_static] gtfs.zip download failed; keeping previously-loaded (stale) schedule");
      return true; // ok to use a stale copy -- matches gova_next_bus.py's ensure_static_zip() trade-off
    }
    Serial.println("[bus_static] gtfs.zip download failed and no schedule loaded yet");
    return false;
  }

  // static, not a stack local -- same reasoning/fix as buildStopResult()'s
  // staticCands array (bus.cpp): this function is already one of several
  // deep in the call chain from setup()/loop() down through an HTTPS fetch
  // (which itself needs real stack headroom for mbedTLS), so every large
  // local this function doesn't strictly need on the stack should stay off
  // it. mz_zip_archive doesn't need per-call/reentrant storage -- it's
  // fully reset via memset on every call anyway.
  static mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  // MUST be set before init -- see zipPsramAlloc()'s comment above (this
  // is the actual fix for the stop_times.txt/calendar_dates.txt heap
  // corruption crash, not just a nice-to-have).
  zip.m_pAlloc = zipPsramAlloc;
  zip.m_pFree = zipPsramFree;
  zip.m_pRealloc = zipPsramRealloc;
  if (!mz_zip_reader_init_mem(&zip, zipBuf, zipLen, 0)) {
    Serial.println("[bus_static] gtfs.zip: not a valid zip archive");
    free(zipBuf);
    return schedule.valid;
  }

  // Built up in a function-static working copy (NOT a stack local -- see
  // bus_static.h's size comment on StaticSchedule, ~42KB fully populated,
  // far more than a typical Arduino task stack) before being committed to
  // `schedule`. A schedule that fails PARTWAY through parsing (e.g.
  // trips.txt ok but stop_times.txt missing) must never silently overwrite
  // a previously-working `schedule`, hence the separate copy + explicit
  // commit only on full success.
  static StaticSchedule fresh;
  fresh.tripCount = 0;
  fresh.stopTimeCount = 0;
  fresh.serviceDateCount = 0;
  fresh.valid = false;

  bool ok = true;
  int idx;
  size_t entrySize = 0;
  void* data = nullptr;

  idx = mz_zip_reader_locate_file(&zip, "trips.txt", nullptr, 0);
  if (idx < 0) { Serial.println("[bus_static] gtfs.zip: trips.txt not found"); ok = false; }
  if (ok) {
    data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &entrySize, 0);
    if (!data) { Serial.println("[bus_static] gtfs.zip: failed to extract trips.txt"); ok = false; }
    else {
      const uint8_t* csv = (const uint8_t*)data;
      size_t csvLen = entrySize;
      stripBom(&csv, &csvLen);
      ok = parseTripsCsv(csv, csvLen, stops, stopCount, fresh);
      free(data);
    }
  }

  if (ok) {
    idx = mz_zip_reader_locate_file(&zip, "stop_times.txt", nullptr, 0);
    if (idx < 0) { Serial.println("[bus_static] gtfs.zip: stop_times.txt not found"); ok = false; }
  }
  if (ok) {
    data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &entrySize, 0);
    if (!data) { Serial.println("[bus_static] gtfs.zip: failed to extract stop_times.txt"); ok = false; }
    else {
      const uint8_t* csv = (const uint8_t*)data;
      size_t csvLen = entrySize;
      stripBom(&csv, &csvLen);
      ok = parseStopTimesCsv(csv, csvLen, stops, stopCount, fresh);
      free(data);
    }
  }

  if (ok) {
    idx = mz_zip_reader_locate_file(&zip, "calendar_dates.txt", nullptr, 0);
    if (idx < 0) { Serial.println("[bus_static] gtfs.zip: calendar_dates.txt not found"); ok = false; }
  }
  if (ok) {
    data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &entrySize, 0);
    if (!data) { Serial.println("[bus_static] gtfs.zip: failed to extract calendar_dates.txt"); ok = false; }
    else {
      const uint8_t* csv = (const uint8_t*)data;
      size_t csvLen = entrySize;
      stripBom(&csv, &csvLen);
      ok = parseCalendarDatesCsv(csv, csvLen, fresh);
      free(data);
    }
  }

  mz_zip_reader_end(&zip);
  free(zipBuf);

  if (!ok) {
    Serial.println("[bus_static] gtfs.zip parse failed partway through; keeping previous schedule (if any)");
    return schedule.valid;
  }

  fresh.valid = true;
  schedule = fresh; // struct copy -- ~42KB, done once per refresh (at most once every 24h), not per-loop
  g_lastFetchMs = now;
  Serial.printf("[bus_static] static schedule refreshed: %d trips, %d stop_times, %d service dates\n",
                schedule.tripCount, schedule.stopTimeCount, schedule.serviceDateCount);
  return true;
}

int staticCandidatesForStop(const StaticSchedule &schedule, const BusStopConfig &stop,
                             time_t nowEpoch, StaticCandidate* out, int maxOut) {
  int n = 0;
  struct tm nowTm;
  localtime_r(&nowEpoch, &nowTm); // relies on the TZ configTzTime() set at boot -- same dependency
                                  // weather_swob.cpp's timestamp handling already has (see its comment)

  // Check yesterday, today, and tomorrow: GTFS represents a post-midnight
  // trip as e.g. "24:06:00" on the PREVIOUS service day, so right after
  // midnight a trip that's actually happening now can only be found by
  // anchoring to yesterday's date -- checking only today+tomorrow computes
  // the NEXT day's occurrence of the same trip_id instead, a full 24h off.
  // This bit gova_next_bus.py once before it was fixed there (see
  // clauderef.md section 5) -- built in here from the start instead.
  for (int dayOffset = -1; dayOffset <= 1; dayOffset++) {
    struct tm dayTm = nowTm;
    dayTm.tm_mday += dayOffset;
    dayTm.tm_hour = 0; dayTm.tm_min = 0; dayTm.tm_sec = 0;
    dayTm.tm_isdst = -1; // let mktime() determine DST for THAT date, not assume today's
    time_t midnight = mktime(&dayTm);
    if (midnight == (time_t)-1) continue; // shouldn't happen; skip defensively rather than guess

    char dayStr[9];
    struct tm dayTmNorm;
    localtime_r(&midnight, &dayTmNorm);
    strftime(dayStr, sizeof(dayStr), "%Y%m%d", &dayTmNorm);

    for (int i = 0; i < schedule.stopTimeCount && n < maxOut; i++) {
      const StaticStopTime &st = schedule.stopTimes[i];
      if (strcmp(st.stopId, stop.stopId) != 0) continue;

      const StaticTrip* trip = findStaticTrip(schedule, st.tripId);
      if (trip == nullptr) continue; // shouldn't happen -- stop_times was already filtered by known trip_ids

      bool routeMatches = false;
      for (int r = 0; r < stop.routeCount; r++) {
        if (strcmp(stop.routes[r], trip->route) == 0) { routeMatches = true; break; }
      }
      if (!routeMatches) continue;

      bool serviceActive = false;
      for (int s = 0; s < schedule.serviceDateCount; s++) {
        if (strcmp(schedule.serviceDates[s].serviceId, trip->serviceId) == 0 &&
            strcmp(schedule.serviceDates[s].date, dayStr) == 0) { serviceActive = true; break; }
      }
      if (!serviceActive) continue;

      StaticCandidate &c = out[n++];
      strncpy(c.tripId, st.tripId, sizeof(c.tripId) - 1);       c.tripId[sizeof(c.tripId) - 1] = '\0';
      strncpy(c.route, trip->route, sizeof(c.route) - 1);        c.route[sizeof(c.route) - 1] = '\0';
      strncpy(c.headsign, trip->headsign, sizeof(c.headsign) - 1); c.headsign[sizeof(c.headsign) - 1] = '\0';
      c.epoch = midnight + st.secondsSinceMidnight;
    }
  }
  return n;
}
