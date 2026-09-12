# Bus data backend — reference for the weather_station project

Written 2026-09-11 for the `weather_station` ESP32-S3 e-ink dashboard project,
which wants to reuse this project's transit-data approach for a real-time bus
departure display (target agency: GO Transit / Metrolinx). Answers the
questions that project's Claude Code session asked; sourced from the actual
code in this repo, not from memory.

## ⚠️ Read this first: agency mismatch

This codebase is built for **GOVA Transit (Greater Sudbury, Ontario)**, not
GO Transit/Metrolinx — two unrelated agencies with unrelated feeds. (The
Android package is even named `com.sudburybus.busstop`.) The *approach*
below (GTFS-Realtime protobuf, static-GTFS fallback, stop/route filtering,
merge strategy) ports directly to any GTFS-RT agency. The *concrete
endpoint URLs and "no auth needed" fact do not* — see below.

## 1. Which API / aggregator does it call?

No aggregator. It hits the transit vendor's feeds directly — **Consat/tmix**,
the real-time backend GOVA (Sudbury) uses. The Python backend
(`backend/gova_next_bus.py`) and the Android Kotlin port
(`android/.../data/BusRepository.kt`) both fetch these same two sources
independently; the Kotlin file is literally commented "port of
backend/gova_next_bus.py, same data sources, same strategy."

## 2. Endpoint URLs + auth

```
static GTFS (zip):         https://sudbury.tmix.se/gtfs/gtfs.zip
GTFS-RT trip updates:      https://sudbury.tmix.se/gtfs-realtime/tripupdates.pb
GTFS-RT vehicle positions: https://sudbury.tmix.se/gtfs-realtime/vehiclepositions.pb
```

**Auth: none.** Just a `User-Agent` header — no API key, no query param, no
OAuth. This is a quirk of this specific small-city agency publishing fully
open feeds; don't assume the same holds for GO Transit. Metrolinx's
real-time/GTFS data is published through their own Developer/Open Data
portal and is expected to require a subscription key (header or query
param) — the exact current endpoint/key mechanics are **not verified in
this codebase** (this agency doesn't use them), so confirm those directly
from Metrolinx's developer portal rather than trusting anything here by
analogy.

## 3. Response format — protobuf, not JSON

**GTFS-Realtime protobuf.** Confirmed via `android/app/build.gradle.kts`
(`org.mobilitydata:gtfs-realtime-bindings:0.2.0`) and the Python side
(`from google.transit import gtfs_realtime_pb2`). Both parse a standard
`FeedMessage` (repeated `FeedEntity` → `TripUpdate` with `StopTimeUpdate`s,
or `VehiclePosition`).

This is the **standard, agency-agnostic GTFS-RT schema** — the same
`gtfs-realtime.proto` published at https://gtfs.org/realtime/reference/
(mirrors Google's original spec). It is not GOVA/tmix-specific in any way,
so whatever agency actually gets integrated, if it publishes GTFS-RT, it's
the same message schema and wire format. Plan on nanopb + real binary
parsing for the real-time part, not ArduinoJson.

The **static** GTFS side (`gtfs.zip`) is the opposite — plain CSVs
(`trips.txt`, `stops.txt`, `stop_times.txt`, `calendar_dates.txt`), zero
protobuf. Much friendlier to parse, but potentially large for ESP32
RAM/flash if a full day's schedule is held in memory — consider parsing
down to only the needed stop_ids/route_ids rather than loading the CSVs
generically.

**Gotcha already hit here:** these CSVs can be UTF-8-with-BOM. The Kotlin
CSV reader didn't strip it and silently broke every row lookup with no
exception thrown. Strip a leading BOM before parsing whatever CSV rows get
kept.

## 4. Stop selection

No geolocation query — a hardcoded config list of `{stop_id, routes[],
label}`, checked directly against the feeds each poll:

```python
STOPS = [
    {"stop_id": "1910", "routes": ["11"], "label": "Lasalle & Notre Dame"},
    {"stop_id": "1820", "routes": ["1N", "105", "106"], "label": "1111 Notre Dame"},
    {"stop_id": "6205", "routes": ["1N"], "label": "Lasalle"},
]
```
(Same shape in `android/.../data/Config.kt`.) For each stop it fans out
over both feeds, filters `TripUpdate`/`StopTimeUpdate` entries matching
that `stop_id` + one of `routes`, merges with the static schedule, sorts by
ETA, and takes the next N.

## 5. Poll interval + quirks (including the real-time "problem")

- GNOME extension: every **15 seconds** (`REFRESH_SECONDS = 15` in
  `extension.js`) — desktop use case, quite aggressive.
- Android widget: every **15 minutes** via WorkManager periodic work —
  Android's practical minimum granularity anyway.
- No rate limiting ever observed from the tmix feed at either cadence. For
  an e-ink dashboard, something well under the GNOME extension's 15s is
  plenty — 30–60s, or tied to the display's own refresh cycle.

**The real "problem" wasn't the API — it was a local Python
packaging/sandboxing bug**, worth knowing conceptually even though it won't
recur in a C++/nanopb build: the GNOME extension's Python venv, when built
from inside a sandboxed dev environment, linked protobuf's compiled C
extension against the sandbox's runtime instead of the host's. Result:
`gtfs-realtime-bindings` silently failed to import at actual runtime (a
different environment than where it was installed), so the live feed
permanently and silently fell back to schedule-only — no exception, no
error, looked exactly like "the real-time feed is down" for a long time
before the real cause was found. The fix that made this visible going
forward was adding an explicit status field to the output:

```json
"realtime_feed": {"ok": true, "feed_time": 1755500000, "error": null}
```

Transferable lesson: **make protobuf parse/library failures loudly visible
in status output — don't let them collapse into a generic "no data yet"
state.** That ambiguity is exactly what burned time here.

Also worth carrying over deliberately: the static+realtime merge has to
check **yesterday, today, and tomorrow** when matching GTFS's post-midnight
time format (`"24:06:00"` etc.), not just today+tomorrow — otherwise
anything still active right after local midnight silently fails to match
and the merge quietly drops the scheduled/live linkage. Bit us once here;
build this in from the start for any date-anchored candidate matching.

## 6. Code / structure to port

The full real-time+merge logic (no secrets in it — this feed has no API
key to redact) is `backend/gova_next_bus.py` in this repo, ~570 lines,
fully commented; the direct Kotlin port is
`android/app/src/main/kotlin/com/sudburybus/busstop/data/BusRepository.kt`.
Both are small enough to hand over whole. Shape to replicate:

1. Fetch `tripupdates.pb` → parse `FeedMessage` → for each `TripUpdate`,
   fall back to the static `trip_id → route_id` lookup if
   `TripDescriptor.route_id` is blank (tmix leaves it empty; other vendors
   might too) → skip `CANCELED`/`DELETED` trips and `SKIPPED` stop-time
   updates → collect `(trip_id, route, headsign, arrival/departure epoch,
   delay)` for stop-time updates matching the target stop_id.
2. Optionally fetch `vehiclepositions.pb` too — a vehicle already
   `STOPPED_AT` the exact stop overrides the predicted time with "now,"
   catching tripUpdates predictions that lag a few minutes behind reality.
3. Treat the feed as stale (ignore it, fall back to static) if
   `FeedHeader.timestamp` is more than 10 minutes old — a frozen upstream
   feed otherwise looks identical to a healthy live one.
4. Merge with the static schedule keyed by `(trip_id, date)`, realtime
   winning; sort by epoch; take the next N.

No raw sample response is included here — it's binary protobuf, not worth
inlining, and this repo's own feed is the wrong agency to sample from
anyway. Once the actual target agency's feed URL is known, the cleanest way
to see real field shapes is to fetch it once and decode it against the
public `gtfs-realtime.proto` (e.g. `protoc --decode`), or point Python's
`gtfs-realtime-bindings` at it interactively.
