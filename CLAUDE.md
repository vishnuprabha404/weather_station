# CLAUDE.md — Home E-Ink Transit & Weather Display (ESP32-S3 / LILYGO T5 4.7")

## Project Status (read this first)

**Weather + time + portrait display: DONE and running on real hardware.**
**Bus/transit integration: DEFERRED, not started.**

The original plan targeted bus + weather + time together. Bus was deliberately
deferred early on to get weather+time working end-to-end first; that milestone
is now complete, including several rounds of on-hardware layout iteration and
two features beyond the original plan (true portrait mode, and a
change-only weather redraw). See "Next Immediate Task" at the bottom for
where to pick this back up.

**Actual project location:** `/home/vishnu/Documents/projects/weather_station/`
(moved from its original `/home/vishnu/Documents/esp 32/weather_station/`
location when this became a git repo, to get it alongside this user's other
projects and out of a path with a space in it). Flat directory, not the
`eink-dashboard-esp32/src/...` layout originally sketched below in
"Suggested Firmware Structure" — that section is kept for historical
context but the *actual* structure is documented in its own section
further down. Git repo, pushed to a private GitHub remote.

## Project Overview

Build a small, always-on home e-ink dashboard that displays:

- Outside temperature / weather information — **done**
- Current date/time — **done**
- Nearby bus departure times (real-time, from a transit API — e.g. GO Transit / Metrolinx) — **not started**
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

Footer (2-column grid, vertical divider between):
- Humidity (icon + label + value)
- Wind (icon + label + speed + compass direction, 2 lines)

Bottom-left, small (20px) de-emphasized text: "Weather updated at HH:MM AM/PM"
— deliberately pulled out of the footer grid (a 3-column footer with a "Last
Update" cell was tried first and dropped per user feedback).

Tapping the main weather block navigates to a full-screen "Today's Overview"
page (high/low, sunrise/sunset, max wind, precip chance) with a back button;
this is always a full refresh, not partial (different layout entirely).

Layout constants live at the top of `renderer.cpp` and were tuned against
real hardware photos across several iterations — treat any specific pixel
number there as "best current estimate, verified against at least one real
photo," not as derived from first principles.

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

**Date-line descenders slowly fading, caused by the minute-tick clear
region (`renderer.cpp`'s `DATE_PY1`/`TIME_PY0` constants).** Found from a
hardware photo: the tail of a descender letter on the date line (e.g. the
'y' in "Monday") was visibly fading over time, specifically correlated with
minute ticks, not date changes. Root cause: `DATE_PY1` and `TIME_PY0` were
the literal same value (`TIME_TEXT_Y - 5`) — a zero-buffer shared boundary
between the date row's region and the time row's per-minute clear region.
A descender dipping down to or past that shared line gets wiped by every
minute tick's `epd_clear_area()` call (which starts exactly there) but
never redrawn, since the minute tick only redraws time digits, not date
text — so it erodes one tick at a time until the date itself next changes
and gets a full redraw. Fixed by giving `TIME_PY0` a few px of headroom
below `DATE_PY1` instead of sharing its value, so the per-minute clear
region no longer reaches into where a date-line descender can land. Same
category of lesson as the bug above: two independently-triggered partial-
refresh regions sharing an exact pixel boundary is fragile — real glyph ink
doesn't respect the nominal layout box a font's advance/position implies,
so adjacent partial-refresh regions need a genuine buffer, not just
touching edges. If time digits ever show their own top-edge ghosting after
this fix, `TIME_PY0` was pushed down too far and needs dialing back
partway — like the rest of this file's layout constants, treat this exact
number as "best estimate from one hardware photo," not final.

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

## Real-Time Bus Data — Feasibility (Confirmed Doable, not yet started)

Real-time transit API polling over Wi-Fi from an ESP32-S3 is proven and well-supported:

1. ESP32-S3 connects to Wi-Fi (built-in radio).
2. `HTTPClient` makes a periodic HTTPS GET request to the transit API (target: GO Transit / Metrolinx).
3. `ArduinoJson` parses the JSON response.
4. Parsed departure times are stored in a small in-memory struct.
5. Renderer draws departures to the e-paper display.
6. Repeats on the refresh interval (target: every 1 minute for bus data).

**Open item to verify before writing firmware:** confirm whether the GO Transit / Metrolinx public API returns plain JSON (straightforward with ArduinoJson) or GTFS-Realtime protobuf (binary — would need the `nanopb` library and more parsing work). Check official Metrolinx developer docs for the exact endpoint/auth method before building the `bus` module.

**HTTPS/TLS note:** ESP32's `HTTPClient` needs either a root CA certificate bundle for the API's HTTPS endpoint, or `setInsecure()` as a quick-start fallback (not recommended long-term). Confirm the API's cert chain when implementing. (Open-Meteo's weather integration still uses plain HTTP, so this remains unhandled there — but both Environment Canada integrations (`weather_ec.cpp`'s citypage feed and `weather_swob.cpp`'s SWOB feed) hit HTTPS endpoints, via `WiFiClientSecure` + `setInsecure()`, i.e. the quick-start fallback, not a pinned cert. Same trade-off will apply to the bus API whenever that's built — revisit all of these together if this ever needs to be hardened.)

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
- ⬜ Bus API unavailable — not applicable yet, bus not started

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
    │                           state machine, all scheduling/timing logic
    ├── config.h              — gitignored, real WiFi creds + location + refresh interval
    ├── config.h.example      — checked-in template
    ├── models.h              — WeatherData struct
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
    ├── display.h / .cpp      — thin wrapper: init/framebuffer/clearBuffer/fullRefresh
    ├── renderer.h / .cpp     — all drawing; portrait coordinate math lives here
    ├── icons.h               — auto-generated (~750KB), see "Asset Generation Pipeline"
    └── portrait_font.h       — auto-generated (~540KB), see "Asset Generation Pipeline"

This is flatter than the `src/`-based layout originally sketched for the
bus+weather combined project — reasonable to keep flat for now; revisit if
the bus module makes the folder too cluttered.

## Configuration

Do not hard-code API keys, Wi-Fi credentials, or private endpoints. Kept in
`config.h` (gitignored — though note: **this isn't a git repo yet**, so
"gitignored" is currently just a convention to honor if/when `git init`
happens, not an enforced protection). Checked-in `config.h.example` shows
the expected shape.

Current fields: `WIFI_SSID`, `WIFI_PASSWORD`, `WEATHER_LATITUDE`,
`WEATHER_LONGITUDE`, `WEATHER_LOCATION_NAME`, `WEATHER_EC_SITE_ID`
(Environment Canada citypage site code, condition text/icon only — see
"Weather Data Sources"), `WEATHER_SWOB_STATION_ID` (Environment Canada SWOB
station `tc_id`, the fresher per-minute source for temp/feels-like/
humidity/wind — same physical station as `WEATHER_EC_SITE_ID` but a
different identifier format; see "Weather Data Sources"),
`WEATHER_REFRESH_MINUTES` (background check cadence — see "Refresh
Strategy" for why this is no longer the same thing as "how often the
screen updates").

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
8. ⬜ Confirm GO Transit / Metrolinx API format (JSON vs GTFS-RT) — **not started**.
9. ⬜ Build `bus` module — **not started**.
10. ⬜ Wire bus into scheduling/rendering — **not started**.
11. ⬜ Solder 40-pin header / design enclosure — optional, later, not started.

## Known toolchain gotchas (Fedora, confirmed during setup)

- Arduino IDE: use the **AppImage**, not Flatpak (LilyGo EPD47's own README flags Flatpak sandboxing as causing serial-port/library issues). AppImage needs `fuse`/`fuse-libs` (`sudo dnf install fuse fuse-libs`).
- Board package: install **"esp32" by Espressif Systems** specifically — a similarly-named "Arduino ESP32 Boards" (by Arduino) package also shows up in Boards Manager and is the wrong one.
- `pyserial` isn't always present: `sudo dnf install python3-pyserial` if upload fails with `ModuleNotFoundError: No module named 'serial'`.
- Serial port permissions reset on every re-enumeration (each upload). A `dialout`-group fix alone needs a relogin and can still be flaky across re-enumeration; a **udev rule targeting the exact vendor/product ID (303a:1001)** is the durable fix.
- Upload failing for no obvious reason: hold **BOOT (IO0)**, press+release **RST**, release **BOOT**, click Upload again — forces bootloader mode manually (S3's auto-reset doesn't always trigger it).

## Next Immediate Task

Weather + time + portrait display is done and verified on real hardware
through several rounds of photo-driven iteration. The next major milestone,
per the original project scope, is bus/transit integration:

1. Confirm the exact GO Transit / Metrolinx API shape (JSON vs GTFS-RT) and auth method.
2. Decide whether/how to reuse logic from the user's existing Android widget / GNOME extension for bus data, per the original project goal of not building a second parallel bus pipeline.
3. Build the `bus` module (`bus.h`/`bus.cpp`), following the same pattern as `weather.cpp` (separate fetch/parse module, never touching rendering or driver code directly).
4. Add a bus section to the portrait layout and wire a 1-minute refresh cadence for it, reusing the same "only redraw if actually changed" discipline already proven out for weather.

If instead the next session is more display polish (real-hardware layout
tuning, new pages, etc.), treat the "Actual Display Layout" and "Asset
Generation Pipeline" sections above as the load-bearing reference — don't
re-derive the portrait rotation math by hand, re-verify computationally
first if it's ever in doubt.
