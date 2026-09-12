# CLAUDE.md — Home E-Ink Transit & Weather Display (ESP32-S3 / LILYGO T5 4.7")

## Project Status (read this first)

**Weather + time + portrait display: DONE and running on real hardware.**
**Bus/transit: a minimal realtime-only "Next bus (ROUTE): N min" line is
DONE and CONFIRMED WORKING on real hardware (2026-09-11).** The full
module (static GTFS schedule + merge + a whole "Next Buses" section,
`bus.cpp`/`bus_static.cpp`) is written and its real bugs are fixed, but is
**currently NOT wired into the sketch** — deliberately parked in favor of
the minimal version after repeated hardware crashes. See "Real-Time Bus
Data" and "Known Bugs Fixed" below before touching any of this.

The original plan targeted bus + weather + time together. Bus was
deliberately deferred early on to get weather+time working end-to-end
first; that milestone is complete, including several rounds of
on-hardware layout iteration and two features beyond the original plan
(true portrait mode, and a change-only weather redraw).

Bus/transit came next, and went through a real journey worth understanding
before continuing it:
1. Built the full module (`bus.cpp`/`bus_static.cpp`/`bus_realtime.cpp` +
   a "Next Buses" section replacing the old footer) — compiled clean, but
   **crashed the board repeatedly** on its first real hardware tests.
   Root-caused two real, since-fixed bugs (see "Known Bugs Fixed"): a
   2016-byte stack frame in `bus.cpp`, and miniz (the zip library backing
   the static GTFS schedule fetch) silently using this chip's tiny
   internal SRAM instead of PSRAM for a 4.78MB file, corrupting the heap.
2. Even with both fixed, getting a clean, confirmed-good hardware run
   proved slow and hard to pin down with confidence (this board's
   bootloader-entry-over-USB flakiness — see "Known toolchain gotchas" —
   made every flash/test cycle its own small ordeal, independent of the
   firmware itself).
3. **Decision: stop debugging the full module blind and start smaller.**
   Reverted `renderer.cpp`/`renderer.h`/`weather_station.ino` to the
   proven pre-bus baseline (git commit before any of this), then added
   back just ONE new thing: `Renderer::drawNextBusLine()` (one line of
   text under the weather block) fed by a small `fetchNextBusLine()` in
   `weather_station.ino` that calls `bus_realtime.cpp`'s realtime feed
   *only* — no `bus_static.cpp`/miniz involved at all, so neither bug
   above is even reachable from this path. **This is what's actually
   flashed and confirmed working right now.**

The full module's source is kept (not deleted) with both bugs fixed, for
whenever this gets built back up incrementally — see "Next Immediate
Task" for how to pick this up without repeating the same mistakes.

**Actual project location:** `/home/vishnu/Documents/projects/weather_station/`
(moved from its original `/home/vishnu/Documents/esp 32/weather_station/`
location when this became a git repo, to get it alongside this user's other
projects and out of a path with a space in it). Flat directory, not the
`eink-dashboard-esp32/src/...` layout originally sketched below in
"Suggested Firmware Structure" — that section is kept for historical
context but the *actual* structure is documented in its own section
further down. Git repo, pushed to a **public** GitHub remote
(`github.com/vishnuprabha404/weather_station` — flipped from private to
public 2026-09-11; `config.h` confirmed never committed, in the full
history, before that happened).

## Project Overview

Build a small, always-on home e-ink dashboard that displays:

- Outside temperature / weather information — **done**
- Current date/time — **done**
- Nearby bus departure times (real-time, from a transit API — **GOVA Transit**, Greater Sudbury's local agency, corrected from an initial GO Transit / Metrolinx guess — see "Real-Time Bus Data" below) — **data layer built (compiles clean, not yet hardware-verified); not yet rendered on screen — see "Planned UI"**
- Potentially additional home-dashboard information later

The project should be inexpensive, reliable, easy to maintain, and visually clean.

The user already has:
- An Android widget that displays bus timing information.
- A GNOME extension that displays bus timing information for nearby bus stops.

A major goal is to reuse the existing bus-data/backend logic where practical rather than creating an entirely separate data pipeline (still applies once bus work starts).

## ⚠️ Hardware Pivot History

This project originally targeted a **Raspberry Pi Zero 2 W + Waveshare e-Paper HAT** running Python on Linux. That plan was abandoned due to a **global Pi Zero 2 W supply shortage**. A Raspberry Pi 4 Model B was briefly considered as a fallback, but the project moved to a **fully integrated ESP32-S3 e-paper board**, which sidesteps the Pi shortage entirely and is a better fit for a low-power always-on dashboard.

**Current hardware is final.**

## Current Hardware

### Board: LILYGO T5-4.7-S3 E-Paper (V2.3)

- **MCU:** ESP32-S3-WROOM-1 (dual-core, WiFi 802.11 b/g/n + Bluetooth 5.0 LE, native USB)
- **Flash:** 16MB
- **PSRAM:** 8MB
- **Display panel:** ED047TC1, 4.7", **960×540, hardwired LANDSCAPE** — confirmed by reading the installed `epd_driver.h` directly: `EPD_WIDTH`/`EPD_HEIGHT` are compile-time constants, and the driver has **no rotation support at all**. The display is now run in portrait orientation anyway (see "Portrait Mode" below) entirely through software — every glyph/icon is pre-rotated offline and placed via a coordinate transform, then the physical board is turned 90° by hand. This was worth re-verifying from source, not assuming.
- **Partial refresh:** Supported, and used extensively (see "Refresh Strategy — Actual Implementation").
- **Touch:** GT911 capacitive touch, 2-point. Used for the tap-to-view daily overview page and its back button.
- **Header pins:** Shipped unsoldered — not needed for this project so far.
- **RTC:** PCF8563 (onboard) — not currently used directly; time comes from NTP (`configTzTime`) instead, since the board is always WiFi-connected.
- **Storage:** onboard microSD/TF card slot — unused, no persistence implemented (in-memory cache only, resets on reboot, by design for this milestone).
- **Power:** USB-C, permanently powered. Li-Po connector unused.
- **GPIO:** 40-pin header, ESP32-S3-specific pinout (`utilities.h` in the LilyGo EPD47 library gives `BOARD_SDA`/`BOARD_SCL`/`TOUCH_INT` etc. for this exact board/revision).

### No separate display or compute board needed
This board is the full compute + display unit.

## Software / Firmware Stack (as actually built)

| Layer | Choice |
|---|---|
| Language | C++ (Arduino framework) |
| OS | None — bare-metal firmware via Arduino core for ESP32, **pinned to 2.0.14/2.0.15** (3.x breaks the LilyGo EPD47 driver calls) |
| Dev workflow | Arduino IDE 2.x (AppImage, not Flatpak — the EPD47 library's own README flags Flatpak sandboxing issues), flash + Serial Monitor directly on-device |
| HTTP + JSON | `HTTPClient` + `ArduinoJson` v7 (`JsonDocument`, elastic allocation) |
| Weather API | **Triple-source, split by field** — Environment Canada's **SWOB** real-time feed (`api.weather.gc.ca`, `swob-realtime` collection, HTTPS/JSON) for temp/feels-like/humidity/wind/last-updated (raw per-minute station telemetry — fresh enough to actually track the 10-minute check cadence); Environment Canada's **MSC GeoMet citypage** API (`citypageweather-realtime` collection) for just the condition text/icon (same station, but only updates hourly); **Open-Meteo** (plain HTTP/JSON, no API key) for the daily-overview page (high/low/precip%/sunrise-sunset) and `isDay`. See "Weather Data Sources" below for why it's split this way. |
| E-paper driver | `LilyGo EPD47` Arduino library (`epd_driver.h`/`.c`) |
| Touch driver | SensorLib's `TouchDrvGT911`, via the non-deprecated `TouchDrv.hpp` header |
| Bus API | **GOVA Transit (Consat/tmix)** — GTFS-Realtime protobuf (`tripupdates.pb`/`vehiclepositions.pb`) via `Nanopb` 0.4.9.1, plus the static GTFS schedule zip (`gtfs.zip`) via `Miniz` 3.1.2 + a hand-written CSV parser. Both libraries manually vendored under `~/Arduino/libraries/`, not from Library Manager. See "Real-Time Bus Data" below. |
| Config | `config.h` (gitignored) + checked-in `config.h.example` template |

## Portrait Mode — how it actually works (read before touching `renderer.cpp`)

The panel is a fixed `EPD_WIDTH=960 × EPD_HEIGHT=540` **landscape** framebuffer
with zero rotation support in the driver. Portrait mode is achieved entirely
in software plus one physical action:

1. Every glyph (both UI fonts and the big-digit temperature font) and every
   icon bitmap is **pre-rotated 90° offline** (Python/Pillow, `transpose(Image.ROTATE_90)`)
   before being baked into a C header as packed 4bpp data.
2. All drawing code in `renderer.cpp` works in **portrait coordinates**
   (`px` 0..539 horizontal, `py` 0..959 vertical) and converts to the
   panel's native landscape coordinates via a **verified** transform:
   - Point transform: native `(x, y) = (py, 539 - px)`.
   - Placing a pre-rotated bitmap whose *original* top-left should land at
     portrait `(px, py)`: `nativeX = py`, `nativeY = 539 - px - rotatedHeight + 1`
     (the `-rotatedHeight+1` term only applies to bitmap placement, not to
     raw rectangles — rectangles use the plain point transform on both
     corners).
   - Touch coordinates (raw controller reading, native space) → portrait:
     `px = 539 - rawY`, `py = rawX`.
   - This math was originally derived by hand and got the sign/axis wrong
     multiple times; it was only trusted once verified computationally
     against a known-correct whole-array rotation
     (`[[1,2],[3,4]] → [[3,1],[4,2]]`) in a standalone Python check. **Always
     re-verify computationally before generating new rotated assets** —
     don't re-derive this by hand from scratch.
3. **The physical board itself must be turned 90° by hand** — there's no
   in-firmware rotation switch. Two physical orientations exist for "turned
   sideways"; if content comes out upside-down or mirrored, turn it the
   other way instead of treating it as a code bug.

## Asset Generation Pipeline (icons + fonts)

Two auto-generated header files hold every non-trivial visual asset, both
regenerated by standalone Python/Pillow scripts (not checked into the
project itself — they live in the working scratchpad and should be copied
somewhere durable if this project continues, since they're needed for any
future visual change):

- **`icons.h`** — weather condition icons (sun/moon/partly-cloudy/cloud/rain/
  snow/storm, each with day+night variants where relevant), small inline
  icons (wind, humidity, sunrise, sunset, location pin, clock), and the
  **big-digit font** used for the large temperature readout (digits 0-9,
  `.`, `-`, and `C` only, ~120px original glyph height).
- **`portrait_font.h`** — a full alphanumeric UI font (A-Z, a-z, 0-9, and
  punctuation actually used: `,.:%-/'<` and space), generated at **three
  sizes**: regular body text (34px, `glyph_*`), small/de-emphasized text
  (20px, `sglyph_*`, used for the bottom "Weather updated at ..." line), and
  medium (50px, `mglyph_*`, used only for the time so it reads larger than
  the date above it).

Both generators share the same core logic:
- Every glyph/icon is rendered upright first, then rotated 90° and packed
  into 4bpp (`pack_4bpp`: nibble = pixel>>4, 0=black..15=white, two pixels
  per byte, even index → low nibble).
- Each emitted symbol carries **both** its stored/rotated dimensions
  (`*_width`/`*_height` — what you pass to the blit call) and its
  **original**, pre-rotation dimensions (`*_owidth`/`*_oheight` — what you
  use for portrait-space layout math, e.g. cursor advance or icon
  footprint). Mixing these up silently misplaces things.
- Glyphs use the font's shared `getmetrics()` ascent/descent for vertical
  placement, **not** each character's own ink bounding box. This was a real
  bug found from a hardware photo: per-glyph bbox anchoring made comma and
  period render as floating marks near the top instead of sitting on the
  baseline, because a low/short glyph's own bbox doesn't reflect where it
  should sit relative to the rest of the line.
- Digits in both fonts are **tabular (monospaced)** — confirmed by checking
  the generated `owidth` values, not assumed. This is what makes the time's
  per-character partial-refresh slot table possible (see below).

Regenerating either file requires re-running the corresponding scratchpad
script; there is currently no build-time hook, it's a manual step whenever a
new glyph, icon, or font size is needed.

## Actual Display Layout (portrait, current)

Header (top of screen):
1. Location pin icon + location name, one line (`WEATHER_LOCATION_NAME` from `config.h`)
2. Date, regular (34px) font
3. Time + timezone, **medium (50px)** font, no border/box around it (a
   bordered "cell" was tried first and dropped — see partial-refresh notes)
4. Horizontal divider

Main block:
- Weather condition icon (day/night variant) + big temperature (custom
  120px-tall digit font) side by side
- Condition text in **UPPERCASE** (source data is Title Case; uppercased at
  render time)
- "Feels like X°C"
- "Wind X km/h DIR" (added 2026-09-11, see below — used to be on its own
  in a separate footer, now a second line right under feels-like)

Bottom-left, small (20px) de-emphasized text: "Weather updated at HH:MM AM/PM"
— its own independently-refreshed line/region (see "Bus section" below for
why that had to change), not part of the footer grid (a 3-column footer
with a "Last Update" cell was tried first and dropped per user feedback,
back when there still was a footer grid at all).

**Bus section (added 2026-09-11, first pass, NOT yet hardware-verified —
see "Planned UI"/"Real-Time Bus Data" → "Actual Implementation")**: the old
Humidity | Wind 2-column footer (with its own divider) is **gone** —
removed to make room for this. Humidity is not shown anywhere on this
screen right now as a result (a known, deliberate, temporary gap until a
dedicated Weather tab exists — see "Planned UI"). In its place: a divider,
a "NEXT BUSES" label (with a small new bus icon — `bus_icons.h`, kept
separate from `icons.h` since that file's own generator no longer exists
anywhere, see its header comment), then one row per configured stop
(`config.h`'s `BUS_STOPS_CONFIG`, currently 3): the stop's label, then up
to 2 upcoming arrivals side by side (route, ETA via `formatBusEta()`, a
`(+N)`/`(-N)` delay suffix only when live and actually delayed, and a
12-hour clock time via `formatBusClock12h()`). A stop with no data at all
shows "No data"; one with data but nothing upcoming shows "No upcoming
buses" — both handled by `BusStopResult.hasAnyData`/`arrivalCount`, not
special-cased in the renderer.

This section is genuinely a first estimate, more so than the rest of this
layout — it was designed against layout MATH (measured font heights /
existing constants), not against a real photo yet, unlike everything else
in this section which already went through that process. **Confirm
against a real hardware photo before treating any of its pixel positions
as settled** — row spacing, column width for the two side-by-side
arrivals, and whether the small font's arrival lines actually fit without
truncating/wrapping are all open questions until then.

Tapping the main weather block navigates to a full-screen "Today's Overview"
page (high/low, sunrise/sunset, max wind, precip chance) with a back button;
this is always a full refresh, not partial (different layout entirely). The
tappable zone is now just the compact weather block (not the bus section
below it — there's no drill-down page for bus data yet, so it's
intentionally non-interactive for now).

Layout constants live at the top of `renderer.cpp` and were tuned against
real hardware photos across several iterations — treat any specific pixel
number there as "best current estimate, verified against at least one real
photo," not as derived from first principles. **Exception: the bus-section
constants (`BUS_*`) are pure estimates, not yet photo-verified — see above.**

## Weather Data Sources — why it's split across THREE APIs (read before touching `weather.cpp`/`weather_ec.cpp`/`weather_swob.cpp`)

The home screen's live weather numbers come from **two different Environment
Canada feeds**, not one, plus Open-Meteo for the daily page. This grew from a
two-source split (EC citypage + Open-Meteo) to three sources after finding
that EC's citypage feed — while numerically trustworthy — only updates about
once an **hour** (it's the station's official transmitted report, not a live
feed), which was noticeably stale against this project's 10-minute check
cadence (a user-visible symptom: the display still showed a 2:00 PM reading
at 2:33 PM). Investigating fixed it, rather than accepting the staleness or
switching to a less-trustworthy source:

- **`weather_swob.cpp` — Environment Canada's SWOB (Surface Weather
  Observations) real-time feed** (`api.weather.gc.ca/collections/
  swob-realtime`). This is the SAME physical station's raw per-**minute**
  sensor telemetry — confirmed live by querying it and inspecting
  consecutive `date_tm-value` timestamps a minute apart — so a 10-minute
  check now genuinely gets ≤10-minute-old data. Owns temperature,
  feels-like, humidity, wind, and `lastUpdated`.
- **`weather_ec.cpp` — Environment Canada's citypage feed** (unchanged
  endpoint, `citypageweather-realtime`). Demoted to owning only the
  human-written condition text + derived icon bucket, since that's the one
  thing SWOB's numeric telemetry has no equivalent for. A stale-by-up-to-
  an-hour condition string ("Cloudy") is far less noticeable than a stale
  temperature, so this is an acceptable trade rather than something to fix
  further.
- **`weather.cpp` — Open-Meteo** (unchanged). Still only the tap-through
  daily-overview page (high/low, precip chance, sunrise/sunset) and
  `isDay` — EC's feeds have no clean numeric precipitation-probability
  field and no day/night flag.

Three independent HTTP calls per refresh cycle now, each capable of failing
without blanking what the others already know.

### `weather_swob.cpp` implementation details

- **Station identifier (`tc_id`), not lat/lon or the citypage site ID.**
  Queried via `GET .../swob-realtime/items?f=json&tc_id-value=<id>&sortby=
  -date_tm-value&limit=1` — server-side filter+sort+limit means the
  response is already just the single latest minute's reading, no client-
  side searching needed. `WEATHER_SWOB_STATION_ID` in `config.h`
  (e.g. `"TSB"` for Greater Sudbury's climate station) — **not** the same
  value/format as `WEATHER_EC_SITE_ID`. Comment in `config.h` explains how
  to look one up for a different location (a small `bbox` query against
  the same collection).
- **Small response, no filter/stream needed.** Unlike citypage's ~90KB
  whole-city-page payload, the server-side `limit=1` here means the
  response is only a few KB (one feature, ~200 flat properties) — so this
  file uses the same simple `getString()` + `deserializeJson()` pattern as
  `weather.cpp`, not citypage's filter+stream approach.
- **No precomputed feels-like field.** SWOB hands back raw
  `air_temp`/`dwpt_temp`/wind speed only, not a `windChill` value like
  citypage did. `weather_swob.cpp` computes wind chill itself using EC's
  official formula (`13.12 + 0.6215T − 11.37V^0.16 + 0.3965TV^0.16`, valid
  ≤10°C and ≥4.8 km/h) and humidex from dewpoint (same Masterton &
  Richardson formula `weather_ec.cpp` used to use), with the same
  "only show it if it'd actually read differently" thresholds as before.
- **UTC timestamps, same hand-rolled conversion as before.** SWOB's
  `date_tm-value` is UTC (`...Z`); `utcIso8601ToLocalHHMM()` +
  `daysFromCivilUTC()` (Howard Hinnant's civil-to-days formula) were moved
  here from `weather_ec.cpp` — NOT `timegm()`, which this project's pinned
  ESP32 Arduino core (2.0.14/2.0.15) doesn't provide (a GNU/BSD libc
  extension, not part of that toolchain's newlib build; found by an actual
  compile error, not assumed). Relies on the `TZ` environment set once by
  the `.ino`'s `configTzTime()` call at boot, same as the clock display.
  **Same known pre-existing gap as before, not introduced by this**:
  `configTzTime()` only runs inside `setup()`'s WiFi-connected branch, so
  if WiFi isn't up at boot, `TZ` never gets set for the process's life.
  `weather_swob.cpp` now owns `WeatherData.lastUpdated` (not
  `weather_ec.cpp` any more) specifically because it's the fresher of the
  two timestamps — showing citypage's hourly one would have kept looking
  stale even though the panel now genuinely refreshes far more often.
- **HTTPS, same `setInsecure()` trade-off as citypage** — see the
  "HTTPS/TLS note" above.

### `weather_ec.cpp` implementation details (now condition-text-only)

- **Site identifier, not lat/lon.** Unchanged: `WEATHER_EC_SITE_ID` in
  `config.h` via `GET .../items/{id}?f=json`.
- **Filter narrowed further.** Since only `properties.currentConditions.
  condition` is read now (not the whole current-conditions block), the
  ArduinoJson deserialization filter was tightened to that one leaf — still
  parsed from `http.getStream()` since the underlying response is still the
  same ~85-90KB whole city page.
- **No WMO codes.** EC gives free-text conditions (`"Mostly Cloudy"`,
  `"Chance of Flurries"`, etc.), not a numeric weather code.
  `ecConditionToIconCode()` substring-matches that text into the *same*
  bucket codes `iconForWeather()` in `renderer.cpp` already switches on, so
  icon-selection logic itself needed zero changes. The literal EC text is
  kept separately in `WeatherData.conditionText` for on-screen display —
  the two fields aren't redundant: two different EC strings can land in
  the same icon bucket (`"Cloudy"` and `"Overcast"` both draw the plain
  cloud icon) but still need to be told apart for `weatherDisplayChanged()`'s
  change detection, which compares `conditionText` too, not just
  `weatherCode`.
- No longer touches temperature/feels-like/humidity/wind/lastUpdated at
  all — `computeFeelsLikeC()`, the dewpoint/windChill parsing, and the
  UTC-timestamp conversion all moved to (or were reimplemented in)
  `weather_swob.cpp` and were deleted from this file rather than left as
  dead code.

### Merge (`weather_station.ino`)

- **No current-conditions day/night flag in either EC feed.** `isDay` is
  still Open-Meteo-owned, same as before.
- **Field ownership on merge, three-way now.** `fetchAllWeather()` seeds a
  working copy from the last-known-good `WeatherData`, then calls all three
  fetchers against it (each only overwrites the fields it owns — see the
  comment on `WeatherData` in `models.h`), then explicitly copies out only
  Open-Meteo's daily fields. `merged.valid` is set centrally in
  `fetchAllWeather()` itself (true if ANY of the three succeeded) rather
  than by each individual fetch function, since "do we have something worth
  displaying" is a property of the merge, not of any one source.

## Known Bugs Fixed

**Weather-changed redraw was silently crashing/rebooting the ESP32 instead
of doing a real partial update (`renderer.cpp`'s `portraitFillRect()`).**
Symptom on hardware: what looked like a *full-panel* refresh (whole screen
flashes, header content settles first, then the icon/footer area flashes
again, then everything settles) every ~10 minutes or whenever the weather
actually changed — i.e. exactly the cadence and trigger of the background
weather check, not of the real 6-hour anti-ghosting timer. That was the
giveaway: nothing in the intentional full-refresh logic runs on a 10-minute
cadence, so a full-refresh-*looking* event tied to weather changes had to be
coming from somewhere else.

Root cause: `drawWeatherPartial()` calls `drawWeatherValues(weather, NULL)`
(no framebuffer — this is the partial-update path, meant to draw straight to
the panel via `epd_draw_grayscale_image()`, which needs no framebuffer
pointer). But `drawWeatherValues()` also redraws the footer's vertical
divider via `portraitFillRect()`, which — unlike every bitmap-drawing helper
in this file — unconditionally called `epd_fill_rect(..., fb)`. Confirmed by
reading `epd_driver.c` directly: `epd_fill_rect()` → `epd_draw_vline()` →
`epd_draw_pixel()` dereferences the `framebuffer` pointer with **no NULL
check at all**, unlike the bitmap path which has a real hardware-direct
fallback. So every single weather-changed redraw wrote through a null
pointer, hard-faulted, and rebooted the chip — and what actually appeared on
screen afterward was `setup()`'s own boot-time `doFullRefresh()`, a real
full-panel redraw, which explains why it looked like "a full refresh" (it
was one) tied to weather changes (the only thing that ever called the
broken path).

Fix: `portraitFillRect()` now branches like `drawPortraitBitmap()` already
did — with a real `fb`, fill into it as before; with `fb == NULL`, build a
tiny solid-color scratch buffer (4bpp, matching this project's own asset-
packing convention — see "Asset Generation Pipeline") and push it directly
via `epd_draw_grayscale_image()` instead. `renderer.cpp` needed two new
includes it was previously getting away without (`<Arduino.h>` for
`Serial`, `<stdlib.h>` for `malloc`/`free`) since — unlike `weather.cpp`/
`weather_ec.cpp`/`weather_swob.cpp` — nothing else in this file transitively
pulls in `Arduino.h` via `<WiFi.h>`.

**Lesson for future partial-draw code in this file:** any drawing primitive
called with `fb` potentially NULL needs an explicit hardware-direct branch
— it is NOT safe to assume the driver's own functions no-op or degrade
gracefully on a null framebuffer. Audit any *new* `epd_*` call added to a
function that can be invoked from a partial-update path (i.e. anything
reachable from `drawTimePartial()`/`drawDatePartial()`/
`drawWeatherPartial()`) the same way before trusting it.

**Three real bugs found crashing the full bus module on its first real
hardware tests (2026-09-11) — root-caused and fixed in the source, but the
module was then parked in favor of a smaller working version rather than
chasing a fourth. Read this before ever re-enabling `bus.cpp`/
`bus_static.cpp`.**

Symptom each time: the board appeared to "freeze" at a stale clock value
forever (a photo showed a real but stale time that never advanced). E-ink
retaining its last image made this look like a hang; it was actually a
crash-reboot loop happening fast enough that no *new* full draw ever
completed to overwrite the stale one — each reboot re-ran `setup()`, got
partway through, crashed again. Diagnosis was entirely remote (no physical
access to the board): confirmed it wasn't a genuine hang by polling
`lsusb` every ~1.5s and watching the board's native-USB device
(`303a:1001`) actually disappear and reappear (proof of real resets), then
captured crash dumps by auto-reconnecting a raw serial read across those
resets. Backtraces from a corrupted stack are useless (nanopb/miniz-era
crashes showed `|<-CORRUPTED`), so root-causing leaned on
`xtensa-esp32s3-elf-objdump` against a locally-built ELF with identical
source (same toolchain, `arduino-cli` compiling the same on-disk files —
build timestamp differs so the printed "ELF SHA256" never matches, but
code addresses do) to read real `entry a1, N` stack-frame sizes (Xtensa's
prologue opcode) instead of guessing from `sizeof()`.

1. **`bus.cpp`'s `buildStopResult()` had a 2016-byte stack frame** — by
   far the largest anything in this project had put on the stack — from
   `StaticCandidate staticCands[BUS_MAX_STATIC_CANDIDATES]` being a plain
   stack-local array. The comment on its neighbor, `merged`, already
   explained why arrays this size need `static` instead; that reasoning
   just wasn't applied to `staticCands` too. **Fix:** made it `static`.
   Confirmed via disassembly: the frame shrank enough for the compiler to
   fully inline the function into `fetchAllBuses()` (192 bytes combined).
2. **miniz defaults to plain `malloc()`/internal SRAM for every
   allocation** (confirmed by reading `mz_zip_reader_init_internal()`/
   `mz_zip_reader_extract_to_heap()` directly) unless its archive's
   `m_pAlloc`/`m_pFree`/`m_pRealloc` are set explicitly — which
   `bus_static.cpp` never did. GOVA's real `stop_times.txt` decompresses
   to **~4.78MB** (confirmed by actually downloading and unzipping the
   real feed, not estimated) — wildly larger than this chip's few hundred
   KB of internal SRAM. The failed/degenerate allocation corrupted the
   heap badly enough to crash *later*, in a completely unrelated place
   (the WiFi driver's own packet-receive path, mid-allocation) — a classic
   "crash site is the victim, not the cause" signature; the giveaway was
   the faulting address `0x0a0a000b` — `0x0a` is literally the ASCII
   newline character, pointing straight at CSV text overwriting something
   it shouldn't. **Fix:** `zipPsramAlloc`/`zipPsramFree`/`zipPsramRealloc`
   (bus_static.cpp) route every miniz allocation through
   `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` instead, set on the archive
   *before* `mz_zip_reader_init_mem()`.
3. **miniz's own internal decompression routine needs far more stack than
   this chip's entire default task stack.** Even after fixes 1-2, the
   board kept crash-looping — sometimes the same heap-corruption
   signature, sometimes an explicit `***ERROR*** A stack overflow in task
   loopTask has been detected`. `xtensa-esp32s3-elf-objdump` on
   `mz_zip_reader_extract_to_mem_no_alloc1$part$8` showed a **9632-byte
   single stack frame** (real DEFLATE decompression state), on top of
   ~1200 more in its caller — comfortably more than this Arduino core's
   entire default 8192-byte task stack (confirmed by reading `main.cpp`
   directly), regardless of anything in this project's own code. Not
   fixable inside miniz's logic; the standard Arduino-ESP32 fix is
   `SET_LOOP_TASK_STACK_SIZE(32768)` (a real macro this core's `Arduino.h`
   provides for exactly this) at file scope in `weather_station.ino`.

**All three are fixed in the source** (`bus.cpp`, `bus_static.cpp`,
`weather_station.ino`'s `SET_LOOP_TASK_STACK_SIZE`), verified individually
via disassembly/direct source reading, **but the full module with all
three fixes together was never actually confirmed clean on real
hardware** — getting a trustworthy multi-minute hardware run proved slow
given this board's separate, compounding bootloader-entry flakiness (see
"Known toolchain gotchas"), and rather than keep debugging blind, the
decision was made to fall back to a much smaller working version instead
(see "Project Status" above). Anyone re-enabling `bus.cpp`/
`bus_static.cpp` should re-verify all three fixes are still in place and
get a real multi-minute clean hardware run before trusting it.

**Lesson for any future bus/renderer code:** the "must be `static`, never
a stack local" rule (see `StaticSchedule`'s own comment in bus_static.h)
applies to *any* array whose element count × element size gets into the
hundreds of bytes — audit this explicitly for new code, don't assume "I
already did the big ones." And for any third-party library pulled in
later: check its actual stack/allocation behavior (objdump the real
`entry` sizes, grep for its own default allocator) rather than assuming
it's well-behaved by default.

## Known Open Issues

**Date-line descenders slowly fading, caused by the minute-tick clear
region (`renderer.cpp`'s `DATE_PY1`/`TIME_PY0` constants) — second attempt
made, NOT yet confirmed on hardware.** Found from a hardware photo: the
tail of a descender letter on the date line (e.g. the 'y' in "Monday")
visibly fades over time, specifically correlated with minute ticks, not
date changes. Root cause: `DATE_PY1` and `TIME_PY0` are the literal same
value (`TIME_TEXT_Y - 5`) — a zero-buffer shared boundary between the date
row's region and the time row's per-minute clear region. A descender
dipping down to or past that shared line gets wiped by every minute tick's
`epd_clear_area()` call (which starts exactly there) but never redrawn,
since the minute tick only redraws time digits, not date text — so it
erodes one tick at a time until the date itself next changes and gets a
full redraw.

**First fix attempt** gave `TIME_PY0` a few px of headroom below
`DATE_PY1` instead of sharing its value. **Reverted** (confirmed on
hardware): that region also has to fully cover the medium font's real ink
at the TOP of the time digits, and pushing it down clipped that instead —
changed digits left a leftover line/ghost of the PREVIOUS digit's top
edge, since the old glyph's topmost row(s) were no longer inside what gets
cleared before the new one is drawn. Traded one visible bug for another.

**Second attempt** (current state): rather than moving the shared boundary
itself, moved the two things on either side of it further apart —
requested independently by the user for layout/spacing reasons (date
pulled closer to the location line, time pushed down for more breathing
room, which also pushes the weather section down a bit). `DATE_PY` went
from 70 to 62 (closer to the location row above it) and `TIME_TEXT_Y` went
from 115 to 130 (more separation from the date line, and `MAIN_TOP`/the
whole weather section shifts down with it automatically since those are
computed from `TIME_TEXT_Y`). `DATE_PY1`/`TIME_PY0` still share the same
formula and still touch at the same relative offset — but the real date
ink now sits much higher (ending around DATE_PY+34+descender ≈ y=100) while
the shared clear boundary is now at y=125, leaving roughly 25px of genuine
clearance that didn't exist before (was 0px). This doesn't touch
`TIME_PY0`'s coverage of the digit glyphs at all, so it shouldn't reintroduce
the first attempt's regression — but this reasoning hasn't been verified
against a real hardware photo yet. **Confirm on hardware before treating
this as closed**: check that (a) the date-line descender no longer fades
over several minute ticks, and (b) time digits still redraw cleanly with
no leftover-line ghosting of their own.

Lesson (still holds): two independently-triggered partial-refresh regions
sharing an exact pixel boundary is fragile — real glyph ink doesn't
respect the nominal layout box a font's advance/position implies. The fix
that actually worked (or should have) was adding real distance between the
CONTENT on either side of the boundary, not shrinking either region's own
necessary coverage.

## Refresh Strategy — Actual Implementation

This differs substantially from the original plan below (which only covered
1-minute bus / 30-60-minute weather). What's actually implemented:

### Time (every real-clock minute, driven by `tm_sec==1`, not a `millis()` interval so it can't drift)
Per-character **slot-based partial refresh** — the time string is a fixed
12-character shape (`HH:MM AP MTZ`, e.g. `"12:49 AM EDT"`), each character
position has a pre-reserved fixed pixel width sized to the widest character
that can ever land there. Every tick, only the characters that actually
changed are cleared and redrawn (a normal minute tick touches exactly one
digit). Falls back to a full-row redraw if the combo string's length is ever
unexpected (defensive only — shouldn't trigger given this project's fixed
timezone strings).

### Date
Partial refresh, but only when the formatted date string actually differs
from what's currently drawn (i.e. once a day, not every minute).

### Weather
**Checked** every `WEATHER_REFRESH_MINUTES` (currently 10, in `config.h`) in
the background, but the panel is only **redrawn** when
`weatherDisplayChanged()` (in `weather.cpp`) says something would actually
look different — compared at *display* precision (temperature/wind rounded
to the same decimal shown on screen, wind direction compared as its 8-point
compass label, not raw degrees) so a fetch that changes only in ways too
small to see never triggers a redraw. This decouples "how often we poll the
API" from "how often we flash the e-ink."

### Bus (added 2026-09-11)
**Fetched** every `BUS_REFRESH_SECONDS` (currently 60, in `config.h`) —
much more often than weather, since a bus countdown is only useful if it's
actually current. But the panel is only **redrawn** when BOTH
`busDisplayChanged()` (bus.cpp — same display-precision idea as weather's,
comparing route/minutes/live-status/delay) says something would look
different, AND a separate `BUS_REDRAW_INTERVAL_MS` throttle (currently 5
minutes, in `weather_station.ino`) has elapsed. That second throttle is
new/different from weather's model: a live countdown ticks down almost
every single fetch even when nothing about the underlying schedule/
realtime actually changed, so `busDisplayChanged()` alone would fire a
redraw nearly every 60-second cycle — a much more aggressive partial-
refresh pattern than anything else in this project. The extra throttle
caps how often the panel actually flashes for bus data specifically,
independent of both the fetch cadence and weather's own redraw cadence.

### Full refresh (anti-ghosting)
Independent 6-hour timer (`FULL_REFRESH_INTERVAL_MS`), unrelated to the
above — partial refresh never fully "cleans" an e-ink panel, so ghosting
slowly accumulates regardless of how often weather/time actually change.
This is a normal full refresh, not the heavier `screen_repair` flash-cycle
example from the LilyGo library (that's a manual fix for when ghosting has
already gotten visibly bad, not part of the routine cycle).

### Daily overview page
Always a full refresh (different layout entirely from the home screen), on
tap only.

## Real-Time Bus Data — Feasibility + Actual Implementation

### ⚠️ Agency correction (2026-09-11): GOVA Transit, not GO Transit/Metrolinx

The original plan (and every mention below of "GO Transit / Metrolinx") was
an initial guess made before the user's actual local transit data was
investigated. It was **wrong**. The user's existing Android widget and GNOME
extension — the ones this project's goal already says to reuse rather than
build a second pipeline — are for **GOVA Transit (Greater Sudbury, Ontario)**,
an unrelated agency with an unrelated feed (Android package is literally
`com.sudburybus.busstop`). This lines up with `config.h.example`, which
already uses Greater Sudbury as its worked example. GO Transit/Metrolinx is
**not** the target agency for this project; treat any remaining "GO
Transit / Metrolinx" text below/elsewhere in this file as superseded
historical context, not a live instruction.

This was confirmed by asking the Claude Code session in the user's Android
widget project directly (relayed by the user, not fetched independently by
this project) and cross-checking its answer against that project's actual
source (`backend/gova_next_bus.py`, `android/.../data/BusRepository.kt`,
`android/.../data/Config.kt`, `android/app/build.gradle.kts`), not taken on
faith. Full detail is preserved in `clauderef.md` (checked into this repo,
no secrets in it — the feed needs no API key) — read that file before
writing `bus.cpp`, don't re-derive any of this from scratch:

- **Endpoints (GOVA / Consat-tmix, no auth needed — just a `User-Agent`
  header):**
  `https://sudbury.tmix.se/gtfs/gtfs.zip` (static schedule, CSVs in a zip),
  `https://sudbury.tmix.se/gtfs-realtime/tripupdates.pb` and
  `.../vehiclepositions.pb` (real-time, binary).
  **These specific URLs and the "no auth" fact are GOVA/tmix-specific —
  don't assume either transfers to a different agency if this ever
  changes.**
- **Real-time format is GTFS-Realtime protobuf, not JSON** — the standard
  `gtfs-realtime.proto` schema (https://gtfs.org/realtime/reference/), so
  this is agency-agnostic. This means `ArduinoJson` does **not** apply to
  the real-time feed; plan on the `nanopb` library plus real binary
  parsing (a `.proto`-generated `FeedMessage` → repeated `FeedEntity` →
  `TripUpdate`/`StopTimeUpdate` or `VehiclePosition`). The static GTFS zip
  side is plain CSV (`trips.txt`/`stops.txt`/`stop_times.txt`/
  `calendar_dates.txt`) — friendlier, but watch for a leading UTF-8 BOM
  (bit the Kotlin port once, silently) and mind ESP32 RAM/flash if holding
  more than the filtered rows actually needed.
- **Stop selection is a hardcoded config list** (`{stop_id, routes[],
  label}`), not geolocation. This project ended up using GOVA's own
  example three stops verbatim (`config.h`'s `BUS_STOPS_CONFIG`: `1910`/
  Lasalle & Notre Dame, `1820`/1111 Notre Dame, `6205`/Lasalle) — the user
  confirmed these are the real widget's actual stops, not placeholders, so
  there was nothing to re-derive.
- **Poll interval:** the GNOME extension uses 15s (aggressive, desktop
  use), the Android widget 15min (WorkManager's practical floor); no rate
  limiting observed at either. Something well under 15s is fine for an
  e-ink dashboard — 30–60s, or tied to the display's own refresh cycle,
  same "check often, redraw only on real change" discipline weather
  already uses.
- **Two portable lessons already paid for once, worth building in from the
  start:** (1) treat the feed as stale/fall back to static schedule if
  `FeedHeader.timestamp` is >10 minutes old — a frozen upstream feed
  otherwise looks identical to a healthy one; (2) GTFS's post-midnight time
  format (`"24:06:00"` etc.) needs matching against yesterday/today/
  tomorrow, not just today/tomorrow, or anything active right after local
  midnight silently fails to merge.
- **`backend/gova_next_bus.py`** (~570 lines, fully commented, no secrets)
  in the Android widget project is the reference implementation to port
  the fetch/parse/merge shape from — see `clauderef.md` section 6 for the
  exact steps.

**HTTPS/TLS note:** ESP32's `HTTPClient` needs either a root CA certificate bundle for the API's HTTPS endpoint, or `setInsecure()` as a quick-start fallback (not recommended long-term). Confirm the API's cert chain when implementing. (Open-Meteo's weather integration still uses plain HTTP, so this remains unhandled there — but both Environment Canada integrations (`weather_ec.cpp`'s citypage feed and `weather_swob.cpp`'s SWOB feed) hit HTTPS endpoints, via `WiFiClientSecure` + `setInsecure()`, i.e. the quick-start fallback, not a pinned cert. Same trade-off will apply to the bus API whenever that's built — revisit all of these together if this ever needs to be hardened.)

### Actual Implementation (2026-09-11) — built, compiles clean, NOT yet hardware-verified

The full fetch/parse/merge data layer described above is written across three
new files, following the same one-file-per-source split as the weather
modules — `bus.cpp` never touches protobuf/CSV/HTTP types, same separation
`renderer.cpp` keeps from `weather.cpp`:

- **`bus_realtime.cpp`/`.h`** — fetches + decodes `tripupdates.pb` and
  `vehiclepositions.pb` via nanopb.
- **`bus_static.cpp`/`.h`** — fetches + parses the static `gtfs.zip`
  (`trips.txt`/`stop_times.txt`/`calendar_dates.txt`) via miniz + a
  hand-written RFC 4180 CSV parser (quoted-field/embedded-comma/`""`-escape
  aware — verified correct by hand-tracing all three cases, not just the
  simple unquoted case, since there was no compiler available at the time
  to catch a subtle mistake there).
- **`bus.cpp`/`.h`** — the merge (static seeds the candidate set, realtime
  overlays/wins for the same trip+day, keeping the static time as
  `scheduledEpoch`), day-anchoring, grace period, sort, take-next-N. Also
  owns `formatBusEta()`.
- **`models.h`** — `BusStopConfig` (config shape), `BusArrival`/
  `BusStopResult` (render-agnostic output — no protobuf/CSV types leak
  out, same discipline `WeatherData` keeps).
- **`gtfs_realtime.pb.h`/`.pb.c`** — auto-generated nanopb bindings,
  checked in (same "generated file, regenerate by hand when needed"
  convention as `icons.h`/`portrait_font.h` — see that file's own header
  comment for exact regeneration steps). Generated from a **deliberately
  trimmed** subset of the official `gtfs-realtime.proto` (only the
  messages/fields actually read — protobuf safely ignores anything not
  declared, so this is safe, not a compatibility risk) — source kept at
  `proto_src/gtfs-realtime-trimmed.proto` + `.options`.
- **`~/Arduino/libraries/Nanopb/`** and **`~/Arduino/libraries/Miniz/`** —
  vendored as real Arduino libraries (not checked into this repo, same as
  ArduinoJson/LilyGo-EPD47/SensorLib/Button2) — nanopb 0.4.9.1 (runtime
  only: `pb.h`/`pb_common`/`pb_decode`/`pb_encode`), miniz 3.1.2 (with
  `MINIZ_NO_STDIO`/`MINIZ_NO_TIME`/`MINIZ_NO_ARCHIVE_WRITING_APIS`/
  `MINIZ_NO_ZLIB_APIS`/`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` enabled directly in
  the vendored `miniz.h` — read-only zip access is all this project needs,
  and disabling the zlib-compatible name layer removes a class of possible
  symbol collisions against ESP-IDF's own bundled miniz, which this project
  never calls into but the linker could otherwise see two definitions of).
  **If this project is ever set up on a different machine, these two
  libraries need to be reinstalled** — they aren't part of the git repo.

**Memory-budget decisions (why the numbers in the code are what they are):**
- `FeedMessage.entity` is a **fixed array** (`max_count:200` in the
  `.options` file), not a callback — but `TripUpdate.stop_time_update` (the
  one field that's genuinely unbounded — a trip can have dozens of stops,
  a whole-city feed can have many trips) **is** `FT_CALLBACK`, decoded/
  filtered row-by-row via a nested callback armed on every one of the 200
  entity slots before the single `pb_decode()` call. This hybrid keeps the
  decode simple (one call, plain array iteration afterward) while still
  bounding memory on the one field that could otherwise blow up
  (~200 entities × ~200 bytes ≈ 40KB, PSRAM-backed via
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` — the same pattern
  `display.cpp` already uses for the framebuffer, not a new convention).
- The callback deliberately does **not** resolve which trip/route a
  matched stop_time_update row belongs to at callback time — that would
  require assuming the wire encodes `TripDescriptor` (tag 1) before
  `stop_time_update` (tag 2), which is conventional but not
  protobuf-spec-guaranteed. Instead it records just `{entityIndex, stopId,
  epoch, delaySec}` and a second pass, run AFTER `pb_decode()` fully
  returns, resolves `trip_id`/`route_id`/cancellation state — zero
  assumptions about field order, at the cost of one extra pass.
- `StaticSchedule` (`bus_static.h`) is **~42KB fully populated** (200
  trips + 500 stop_times + 128 service dates) — **never** a stack local
  anywhere in this code (would overflow a typical Arduino task stack);
  always `static`/global, explicitly `memset`/field-reset instead of
  relying on C++ in-class default member initializers (which don't run on
  raw `malloc`'d/reinterpreted memory — only on true constructor-run
  storage). This is the same reasoning nanopb's `FeedMessage` heap buffer
  above needed.

**Deliberate scope simplification vs. the Python reference**
(`gova_next_bus.py`): this port does **not** parse `stops.txt` or track
stop lat/lon. That data only ever fed (a) a `stop_name` fallback label —
unneeded here since `BusStopConfig.label` (config.h) is always provided —
and (b) the purely cosmetic "how far is the live-tracked vehicle from the
stop" figure, which doesn't affect any arrival-time correctness (the thing
that DOES affect correctness — a vehicle already `STOPPED_AT` the exact
stop, overriding the predicted time — is kept, as `stoppedNow` in
`bus_realtime.h`). One fewer CSV file, no lat/lon cross-referencing
complexity, zero loss of anything that changes what time actually shows up
on screen. Flagged here rather than silently dropped — see `bus_static.h`'s
own comment too.

**Verification performed (2026-09-11), all without real hardware:**
- The full sketch (weather + time + portrait + this bus module) **compiles
  clean** against the actual pinned toolchain — found the exact esp32 core
  2.0.15 + xtensa-esp32s3 compiler already installed locally
  (`~/.arduino15/packages/esp32/...`), installed `arduino-cli` to drive it,
  and ran a real `arduino-cli compile`. Zero errors, zero warnings from any
  of the new files (the only warnings anywhere are pre-existing, from
  `LilyGo-EPD47`'s bundled zlib / `SensorLib`'s haptic driver / one
  already-deprecated `getPoint()` call that predates this bus work).
- **Partition scheme matters and was wrong by default**: arduino-cli's
  default FQBN partition scheme is sized for 4MB flash (1.2MB app
  partition) — this board's existing `icons.h` (751KB) + `portrait_font.h`
  (537KB) alone already wouldn't fit in that, so the real Arduino IDE setup
  must already be using something bigger. Recompiling with
  `PartitionScheme=huge_app` (3MB app partition, appropriate for this
  board's actual 16MB flash) gives **38% flash / 43% RAM** with the full
  bus module included. **Open item: confirm in Arduino IDE's board menu
  that Partition Scheme is actually set to Huge APP (or an equivalent
  large-app 16MB scheme) before flashing** — if it's set to something
  smaller, this won't fit.
- What compiling does **NOT** verify: real network behavior (feed sizes
  vs. `FEED_MAX_ENTITIES`/`BUS_MAX_REALTIME_CANDIDATES`/
  `BUS_STATIC_MAX_TRIPS` etc. actually being enough headroom for GOVA's
  real feed), whether tmix's HTTP responses actually send a normal
  `Content-Length` (assumed by `fetchBinary()` in both new `.cpp` files —
  logged as a clean failure if not, not a crash, but unverified against the
  live endpoint), or any of the nanopb nested-callback decode logic against
  real wire bytes. **Treat all of this as "reads correct, verified to
  compile, never run" until proven otherwise on real hardware** — same
  standard this file already holds other unverified work to (see "Known
  Open Issues").

## Planned UI — Multi-Screen Layout (mockup received 2026-09-11)

The user sent a 4-screen mockup (`new layout.png`, in this repo) for the
next round of display work — a genuine multi-screen app, not just a bus
section bolted onto the existing single home screen. **Do not start
building screens 2-4 without re-reading `new layout.png` directly first**
— this section is a planning summary, not a pixel spec (this project's own
"Actual Display Layout" section above shows how much real layout-constant
tuning against hardware photos this kind of thing eventually needs).

### Phase 1 decisions (made 2026-09-11) and status

The user chose a phased approach rather than building all 4 screens at
once — **Phase 1: a compact bus section added to the existing Home screen
only**, no bottom nav, no tabs, no drill-down. **This is now built** (see
"Actual Display Layout"'s "Bus section" and "Real-Time Bus Data" →
"Actual Implementation") but **not yet confirmed on real hardware** —
first-pass layout math only, same caveat as always in this file. Decisions
made alongside that scope choice, all defaults were accepted:
- Mockup's stop names ("Donovan College" etc.) were placeholder — kept the
  real 3 configured stops (`BUS_STOPS_CONFIG`) as-is, just needed real
  labels wired in where the mockup had example ones (done, matches
  `config.h`).
- Battery icon: drop it. Doesn't come up in Phase 1 (no new header chrome
  was added — the existing header is untouched), but applies to any header
  built in a later phase.
- Favorites: skip entirely for Phase 1 (no "All Stops"/"Favorites" toggle
  exists — there's no tabbed Bus Timings screen yet at all).

Screens 2-4 (Bus Timings tab, Route detail drill-down, Weather tab) and the
bottom nav bar are **still not built** — everything below this point is
still a forward-looking plan for THOSE, not a status report on Phase 1.

**The four screens**, tied together by a persistent bottom nav bar (Home /
Bus Timings / Weather, each with an icon):
1. **Home** — location+time header; a condensed current-weather block
   (icon, big temp, feels-like, wind — no humidity/precip here); a "NEXT
   BUSES" list with one card per configured stop, each showing its next TWO
   arrivals side by side (minutes + delay-or-"ON TIME" badge + clock time);
   a "Last updated" line with a refresh icon.
2. **Bus Timings** (tab) — an "All Stops" / "Favorites" toggle, then the
   same per-stop cards as Home but with each arrival as its own full-width
   tappable row (implies tapping a row drills into screen 3).
3. **Route detail** (drill-down, e.g. "Route 11: Donovan College →
   Cambrian") — a "Next Bus" hero card, an "Upcoming Buses" list, and a
   "Route Stops" section: an ordered, connected list of every stop on that
   route (a transit-map-style vertical line with a dot per stop).
4. **Weather** (tab) — big icon/temp/condition, feels-like + wind, and a
   detail grid: Humidity, Precipitation, Pressure, Visibility.

**Remaining mismatches for screens 2-4 — resolve before implementing,
don't silently guess (the stop-name and battery-icon ones are RESOLVED,
see "Phase 1 decisions" above; kept below for the still-open ones):**
- **Pressure and Visibility (screen 4) aren't fetched by anything yet.**
  `WeatherData` (models.h) has no field for either. Open-Meteo's API likely
  has both in its `current` block (worth checking its docs when this is
  built); SWOB may separately have station pressure. Needs a source
  decision + a `weather.cpp`/`weather_swob.cpp` change before this screen
  can show real numbers.
- **Live precipitation % (screen 4) is currently a DAILY-only field.**
  `precipProbabilityMax` (models.h) is explicitly the tap-through daily
  overview's number, sourced once and not intended as a "right now" figure.
  Screen 4 wants it on the main weather tab instead/as well — needs either
  a second field or a repurposing decision, not just wiring the existing
  one in unchanged.
- **"Route Stops" (screen 3) needs the FULL ordered stop sequence for a
  route** — not just the ~3 configured stops. `bus_static.cpp`'s
  `stop_times.txt` parse currently filters down to ONLY rows matching a
  configured `stop_id` (that's what keeps it small/RAM-friendly — see
  "Actual Implementation" above). Building this screen means either
  capturing `stop_sequence` + widening that filter to "every stop_time row
  for any trip on a watched route" (bigger, but still bounded to a handful
  of routes, not the whole city), or a separate, purpose-built fetch. A
  real sizing/architecture decision for whenever this screen gets built,
  not a small tweak.
- **Per-arrival route badges**: `BusArrival.route` (models.h) already
  carries the route id, so screens that want to show it per-row (a stop
  served by multiple configured routes) need no data-layer change — it's
  already there, just not drawn today.

None of the above blocks continuing other work — they're flagged so
whoever builds this (with `new layout.png` open) resolves them
deliberately instead of guessing mid-implementation.

## Reliability Requirements — status

Handled gracefully, without crashing the firmware:
- ✅ Wi-Fi temporarily unavailable — `ensureWiFi()` runs every loop iteration, reconnects without blocking
- ✅ API timeout — `http.setTimeout(HTTP_TIMEOUT_MS)`
- ✅ API rate limiting — any non-200 (incl. 429) hits the same "log, keep old data" path
- ✅ Invalid API response — `deserializeJson` error check + explicit field-presence check before trusting any value
- ✅ Weather API unavailable — `fetchWeather()`/`fetchCurrentConditionsEC()`/`fetchCurrentConditionsSWOB()` each return `false` on failure and leave their fields untouched; `fetchAllWeather()` (the `.ino`) only needs ONE of the three to succeed to have something worth keeping, and never overwrites `currentWeather` on a total failure of all three
- ✅ Missing cached data on first boot — `WeatherData.valid` defaults false; renderer shows "Waiting for first weather update..." instead of zeros
- ⚠️ E-ink refresh failure — not programmatically catchable via the driver API; mitigated by the routine refresh cadence self-correcting any one-off glitch (acceptable, unchanged from original plan)
- ✅ Device reset/power cycle — `setup()` does an immediate full draw with whatever data is available, so the display repopulates within one boot+fetch cycle
- ✅ Bus API/feed unavailable — `fetchRealtimeCandidates()`/`ensureStaticSchedule()` (bus_realtime.cpp/bus_static.cpp) each return `false` on failure and leave prior data untouched; `fetchAllBuses()` (bus.cpp) only needs ONE of the two sources to have anything for a given stop to report data — same "any success counts" contract as weather. **Written and compiles clean; not yet exercised against the real feed on real hardware** — see "Real-Time Bus Data" → "Actual Implementation" for exactly what's unverified.

If a request fails:
1. Keep the last valid in-memory data. **Done.**
2. Optionally show a stale-data indicator. **Not implemented** — known, accepted gap (screen just keeps showing the last successful "Weather updated at" timestamp, which is visible but passive).
3. Retry on the next scheduled cycle. **Done.**
4. Never let one failed API call halt the whole update loop. **Done.**

## Power

Unchanged from original plan — permanently USB-C powered indoors, no battery, no deep-sleep. E-paper panel only draws power during refresh events (bracketed by `epd_poweron()`/`epd_poweroff()` around every refresh, partial or full).

## Actual Firmware Structure

Flat sketch folder (Arduino IDE auto-compiles every `.cpp`/`.h` in the
sketch folder alongside the `.ino` — no separate build system):

    weather_station/
    ├── weather_station.ino   — setup()/loop(), WiFi, NTP, touch init, page
    │                           state machine, all scheduling/timing logic,
    │                           PLUS the minimal fetchNextBusLine() (calls
    │                           bus_realtime.h directly) — see "Project Status"
    │                           for why this lives here and not in bus.cpp
    ├── config.h              — gitignored, real WiFi creds + location + bus stops + refresh intervals
    ├── config.h.example      — checked-in template
    ├── models.h              — WeatherData, BusStopConfig/BusArrival/BusStopResult structs
    │                           (BusArrival/BusStopResult currently unused --
    │                           belong to the parked full module, bus.h/bus.cpp)
    ├── weather.h / .cpp      — fetchWeather() (Open-Meteo: daily page + isDay),
    │                           windDirectionToCompass(), formatTime12h(),
    │                           weatherDisplayChanged()
    ├── weather_ec.h / .cpp   — fetchCurrentConditionsEC() (Environment Canada
    │                           citypage: condition text/icon only) — see
    │                           "Weather Data Sources" above
    ├── weather_swob.h / .cpp — fetchCurrentConditionsSWOB() (Environment
    │                           Canada SWOB: temp/feels-like/humidity/wind/
    │                           lastUpdated, the fresher per-minute source) —
    │                           see "Weather Data Sources" above
    ├── bus_realtime.h / .cpp — fetchRealtimeCandidates() (tripupdates.pb +
    │                           vehiclepositions.pb via nanopb) -- the ONE bus
    │                           module actually called right now (from the
    │                           .ino directly, not through bus.cpp)
    ├── bus.h / .cpp          — fetchAllBuses() (merge: static + realtime,
    │                           day-anchoring, grace/sort/take-N),
    │                           formatBusEta() -- PARKED, not currently called
    │                           from anywhere (see "Project Status"/"Known Bugs
    │                           Fixed") -- still compiled (Arduino builds every
    │                           .cpp in the sketch folder regardless), just unused
    ├── bus_static.h / .cpp   — ensureStaticSchedule() (GOVA's gtfs.zip via
    │                           miniz: trips/stop_times/calendar_dates CSVs,
    │                           filtered to configured stops/routes only) --
    │                           PARKED, same as bus.h/.cpp -- this is the file
    │                           with the two miniz bugs (now fixed in-source,
    │                           never confirmed together on real hardware)
    ├── gtfs_realtime.pb.h/.c — auto-generated nanopb bindings (trimmed GTFS-RT
    │                           schema), see "Real-Time Bus Data" above
    ├── proto_src/            — the trimmed .proto + .options gtfs_realtime.pb.h/.c
    │                           was generated from (regeneration source, not compiled)
    ├── clauderef.md          — reference doc from the user's Android widget
    │                           project's Claude session (GOVA API details);
    │                           informational, not compiled
    ├── new layout.png        — the 4-screen UI mockup, see "Planned UI" above
    │                           (still just a future plan -- nothing built from
    │                           it is currently wired in; see "Project Status")
    ├── bus_icons.h           — auto-generated bus icon bitmap, SEPARATE from icons.h
    │                           (that file's own generator no longer exists anywhere
    │                           on this machine — see bus_icons.h's own header
    │                           comment) -- currently unused (belongs to the
    │                           parked Phase-1 "Next Buses" section, not the
    │                           minimal line actually in use)
    ├── assets_src/           — gen_bus_icon.py, the durable regeneration source for
    │                           bus_icons.h (icons.h/portrait_font.h's own generators
    │                           were never saved anywhere this durable — don't repeat
    │                           that mistake for any future asset)
    ├── display.h / .cpp      — thin wrapper: init/framebuffer/clearBuffer/fullRefresh
    ├── renderer.h / .cpp     — all drawing; portrait coordinate math lives here.
    │                           Back to the proven pre-bus layout PLUS one small
    │                           addition, drawNextBusLine() -- one plain text line
    │                           under the weather block, independently partial-
    │                           refreshed. This is CONFIRMED WORKING on real
    │                           hardware (2026-09-11). The old footer-replacing
    │                           "Next Buses" section this file grew earlier the
    │                           same day is NOT part of the current renderer.cpp
    │                           (reverted) -- its design is preserved above under
    │                           "Actual Display Layout"/"Planned UI" for whenever
    │                           the full module comes back, but don't assume it
    │                           matches the code on disk right now
    ├── icons.h               — auto-generated (~750KB), see "Asset Generation Pipeline"
    └── portrait_font.h       — auto-generated (~540KB), see "Asset Generation Pipeline"

Still flatter than the `src/`-based layout originally sketched for the
bus+weather combined project, but no longer trivially so now that bus has
6 files of its own (3 modules × .h/.cpp) plus generated/reference material
— revisit if the "Planned UI" work adds much more.

Two Arduino libraries this project depends on are **not** part of this git
repo (same as ArduinoJson/LilyGo-EPD47/SensorLib/Button2, all externally
installed): **Nanopb** 0.4.9.1 and **Miniz** 3.1.2, both manually installed
under `~/Arduino/libraries/` (not from Library Manager — see "Real-Time Bus
Data" → "Actual Implementation" for exact versions/config and why). If this
project is ever set up on a different machine, these need reinstalling too.

## Configuration

Do not hard-code API keys, Wi-Fi credentials, or private endpoints, or
anything location-identifying (a stop_id counts — see below). Kept in
`config.h`, which is genuinely gitignored and confirmed never committed
(this repo is now public on GitHub — see "Project Status"/git history, so
this actually matters, not just a convention). Checked-in `config.h.example`
shows the expected shape.

Current fields: `WIFI_SSID`, `WIFI_PASSWORD`, `WEATHER_LATITUDE`,
`WEATHER_LONGITUDE`, `WEATHER_LOCATION_NAME`, `WEATHER_EC_SITE_ID`
(Environment Canada citypage site code, condition text/icon only — see
"Weather Data Sources"), `WEATHER_SWOB_STATION_ID` (Environment Canada SWOB
station `tc_id`, the fresher per-minute source for temp/feels-like/
humidity/wind — same physical station as `WEATHER_EC_SITE_ID` but a
different identifier format; see "Weather Data Sources"),
`WEATHER_REFRESH_MINUTES` (background check cadence — see "Refresh
Strategy" for why this is no longer the same thing as "how often the
screen updates"), `BUS_STOP_COUNT` + `BUS_STOPS_CONFIG` (which GOVA
stop_id(s)/route(s) to show — see "Real-Time Bus Data"; location-
identifying, same reasoning as the weather location fields), and
`BUS_REFRESH_SECONDS` (background check cadence for the bus feeds).

## Coding Principles

- Prefer simple, readable C++ — avoid unnecessary abstraction for a single-purpose embedded firmware.
- Keep API-specific code separate from rendering code. (`weather.cpp` never touches `epd_driver.h`; `renderer.cpp` never touches `HTTPClient`/`ArduinoJson`.)
- Keep the renderer deterministic and easy to reason about.
- Use `Serial.println` logging liberally during development.
- Never commit API keys, Wi-Fi credentials, or `config.h`.
- Comment non-obvious ESP32/e-paper hardware quirks in place — this project leans on this heavily for the portrait-rotation math and the asset pipeline's original/rotated dimension distinction, both of which are easy to get subtly wrong without the comment right there.

## GPIO Notes

- This is the **ESP32-S3** revision of the T5 4.7" — pinout **differs** from the older ESP32-WROVER version of the same board. `utilities.h` (bundled with the LilyGo EPD47 library) has the correct S3-specific pins (`BOARD_SDA`, `BOARD_SCL`, `TOUCH_INT`, etc.) — used directly rather than hand-copying pin numbers.
- Most GPIO pins are claimed by the display and onboard peripherals (touch, RTC, SD card, etc.).

## Development Workflow — what actually happened

1. ✅ Installed Arduino IDE (AppImage) + ESP32 board package (2.0.14/2.0.15, not 3.x) + LilyGo EPD47 library + ArduinoJson v7.
2. ✅ Confirmed board via stock `demo` example.
3. ✅ Confirmed WiFi via `wifi_sync` example.
4. ✅ Built `weather` module (Open-Meteo, `current` + `daily` params).
4a. ✅ Swapped current-conditions source to Environment Canada's MSC GeoMet
    API (`weather_ec` module) after Open-Meteo's temperature was found
    inaccurate; Open-Meteo kept for the daily page only. See "Weather Data
    Sources".
4b. ✅ Added `weather_swob` module (Environment Canada's SWOB per-minute
    telemetry feed) after noticing the EC citypage feed itself only updates
    ~hourly, which was stale against the 10-minute check cadence; citypage
    demoted to condition-text/icon only. See "Weather Data Sources".
5. ✅ Built `display`/`renderer` modules, landscape first, then converted to portrait.
6. ✅ Built the page state machine (`PAGE_HOME`/`PAGE_DAILY`) with touch navigation.
7. ✅ Tuned partial-refresh regions and refresh cadence against real hardware photos, multiple rounds.
8. ✅ Confirm the real transit API/agency and format — **done**: GOVA Transit (Sudbury) via Consat/tmix, GTFS-Realtime protobuf, no auth. See "Real-Time Bus Data" above and `clauderef.md`. (Corrects the original GO Transit / Metrolinx guess.)
9. ✅ Build `bus` module (`bus.h`/`bus_static.h`/`bus_realtime.h` + .cpp) — **done, compiles clean, not yet hardware-verified**. See "Real-Time Bus Data" → "Actual Implementation".
9a. ✅ Received a 4-screen UI mockup (`new layout.png`) for the next round of display work — see "Planned UI — Multi-Screen Layout". Not built; several real mismatches against current config/data flagged there for resolution first.
10. ⬜ Wire bus into rendering — **not started** (currently Serial-logged only from `weather_station.ino`'s `loop()`; no renderer.cpp changes made). Blocked on resolving "Planned UI"'s open items, then designing the actual portrait layout for however many of the 4 mockup screens get built first.
11. ⬜ Solder 40-pin header / design enclosure — optional, later, not started.

## Known toolchain gotchas (Fedora, confirmed during setup)

- Arduino IDE: use the **AppImage**, not Flatpak (LilyGo EPD47's own README flags Flatpak sandboxing as causing serial-port/library issues). AppImage needs `fuse`/`fuse-libs` (`sudo dnf install fuse fuse-libs`).
- Board package: install **"esp32" by Espressif Systems** specifically — a similarly-named "Arduino ESP32 Boards" (by Arduino) package also shows up in Boards Manager and is the wrong one.
- `pyserial` isn't always present: `sudo dnf install python3-pyserial` if upload fails with `ModuleNotFoundError: No module named 'serial'`.
- Serial port permissions reset on every re-enumeration (each upload). A `dialout`-group fix alone needs a relogin and can still be flaky across re-enumeration; a **udev rule targeting the exact vendor/product ID (303a:1001)** is the durable fix.
- Upload failing for no obvious reason: hold **BOOT (IO0)**, press+release **RST**, release **BOOT**, click Upload again — forces bootloader mode manually (S3's auto-reset doesn't always trigger it).
- **This is worse and more persistent than "sometimes needed" — confirmed
  over many flash cycles in one session (2026-09-11).** This board's
  native USB-Serial-JTAG peripheral is flaky about entering AND exiting
  bootloader/download mode via software (DTR/RTS) alone:
  - Entering download mode for an upload: auto-reset (`esptool`'s default,
    what arduino-cli/Arduino IDE normally rely on) frequently fails with
    "Unable to verify flash chip connection (No serial data received)" —
    even immediately after a manual BOOT+RST, and even on an immediate
    retry with no change at all. A physical BOOT+RST has been the only
    reliably-successful trigger found; **holding BOOT continuously through
    the whole upload attempt (release only after upload starts
    succeeding), not just a quick press-release before clicking Upload,**
    was noticeably more reliable than a brief tap — the download-mode
    window seems to close fast.
  - **Exiting bootloader mode afterward is a separate, equally real
    failure mode, not just a formality.** A successful upload's own
    "Hard resetting via RTS pin..." (`esptool`'s `--after hard_reset`, what
    a normal Arduino IDE upload also does) can leave the chip **still
    sitting in the ROM bootloader**, never actually running the new
    firmware at all — indistinguishable from the outside (the display
    just looks unchanged/stale) unless checked directly (`esptool.py
    --before no_reset --after no_reset chip_id`: "Staying in bootloader"
    means stuck; a connection *failure* with that same flag means it's
    actually running — a genuinely confusing inversion the first time you
    see it). Software commands to force an exit
    (`--before no_reset --after hard_reset run`, plain DTR/RTS pulses via
    a raw serial connection) were **not reliable** — only a physical
    **plain RST press** (no BOOT) consistently worked.
  - **Practical effect:** budget a physical button press for essentially
    every upload AND, separately, potentially another one just to get the
    freshly-flashed firmware to actually start running. Don't trust "the
    upload succeeded" or "it hard-reset" as proof the new firmware is
    running — check independently (serial output, or the display actually
    changing) before concluding a flash didn't work when it might just be
    stuck in bootloader.

## Next Immediate Task

Weather + time + portrait display is done and verified on real hardware.
Bus/transit went through a full build-crash-diagnose-fix-park cycle in one
session (2026-09-11) — see "Project Status" and "Known Bugs Fixed" for the
full story — and landed on a **confirmed-working minimal version**: one
realtime-only "Next bus (ROUTE): N min" line, added to the *original*
proven home screen layout (NOT the Phase-1 "Next Buses" section described
under "Planned UI"/"Actual Display Layout" — that section's code still
exists in `renderer.cpp` history but is not what's currently wired up;
`weather_station.ino` currently only includes `bus_realtime.h`, not
`bus.h`/`bus_static.h`).

**What to check first, before building anything further:** confirm the
minimal line keeps working over a longer stretch (it was only confirmed
visually in the moment, not watched for an extended period) and that the
route/minutes shown match reality. Given how much this session's hardware
turnaround cost (mostly the bootloader-entry flakiness in "Known toolchain
gotchas", not the code), don't skip straight to a bigger rebuild without
that longer confirmation first.

**Reasonable next steps, roughly in order of risk:**
1. Show the SAME minimal line for more than one configured stop (still
   realtime-only, still no `bus_static.cpp`/miniz) — a small, low-risk
   extension of what's already proven.
2. Reintroduce the full merge (`bus.cpp`) — still skip `bus_static.cpp`
   for now; a realtime-only merge (no static-schedule fallback, no
   day-anchoring) is a smaller step and already covers "does a bus number
   show up" for most of the day.
3. Only once that's solid, reintroduce `bus_static.cpp` (the static GTFS
   schedule) — with its two already-fixed bugs (PSRAM allocator, and
   `SET_LOOP_TASK_STACK_SIZE` for miniz's own stack needs) still in place
   — and get a real multi-minute clean hardware run before trusting it,
   not just a compile.
4. The Phase-1 "Next Buses" section / full multi-screen mockup (`new
   layout.png`, "Planned UI") comes after the data layer is solid, not
   before — building richer UI on top of an unproven data layer is what
   made this session's debugging hard to pin down.

If instead the next session is more display polish on what's already
proven (real-hardware layout tuning on the header/weather block, the
existing daily-overview page, etc.), treat the "Actual Display Layout" and
"Asset Generation Pipeline" sections above as the load-bearing reference —
don't re-derive the portrait rotation math by hand, re-verify
computationally first if it's ever in doubt.
