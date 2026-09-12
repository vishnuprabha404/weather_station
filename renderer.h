#pragma once
#include "models.h"

// PORTRAIT MODE: the physical panel is a fixed 960x540 landscape buffer with
// no rotation support in the driver, so this renderer draws every glyph and
// icon PRE-ROTATED and places it via a verified 90-degree coordinate
// transform (see icons.h / portrait_font.h generation notes). The panel
// itself must be physically turned 90 degrees to view it correctly — if it
// comes out sideways/mirrored, try turning it the other way (only two
// physical orientations are possible for "turn it sideways").
//
// All coordinates in this module's callers are in PORTRAIT space: px is
// horizontal (0..539), py is vertical (0..959).

namespace Renderer {
  // One-time full-screen draw (chrome: location/dividers/labels, plus
  // whatever values are known at boot). Uses the main framebuffer + a full
  // e-ink refresh. Call once from setup(). `busResults` must have
  // BUS_STOP_COUNT (config.h) entries -- see bus.h's fetchAllBuses().
  void drawFullScreen(const WeatherData &weather, const BusStopResult* busResults, int busCount,
                       const char* timeStr12h, const char* dateStr, const char* tzStr);

  // Partial refresh of ONLY the time+timezone area — does not touch the
  // date or anything else. Call every minute.
  void drawTimePartial(const char* timeStr12h, const char* tzStr);

  // Partial refresh of ONLY the date area — does not touch the time or
  // anything else. Call whenever the date string has actually changed
  // (once a day), not on every minute tick, so it doesn't flash for no
  // reason.
  void drawDatePartial(const char* dateStr);

  // Partial refresh of ONLY the weather info area (icon, temp, condition,
  // feels like, wind, last-updated line) — does not touch the rest of the
  // panel, INCLUDING the bus section below it (that has its own region —
  // see drawBusPartial()). Call whenever fresh weather data arrives and
  // weatherDisplayChanged() says it would actually look different.
  void drawWeatherPartial(const WeatherData &weather);

  // Partial refresh of ONLY the "Next Buses" section — does not touch
  // weather, the header, or anything else. `busResults` must have
  // BUS_STOP_COUNT (config.h) entries. Call whenever fresh bus data
  // arrives and busDisplayChanged() (bus.h) says it would actually look
  // different — see CLAUDE.md's "Planned UI" for why this is throttled to
  // a slower cadence than the underlying fetch (a live countdown changes
  // almost every cycle, unlike weather, so redrawing on every fetch would
  // flash the panel far more than weather ever does).
  void drawBusPartial(const BusStopResult* busResults, int busCount);

  // Full-screen "today at a glance" page: high/low, sunrise/sunset, max
  // wind, precipitation chance, plus a back button. Full refresh — this is
  // a totally different layout from the home screen, not a partial update.
  void drawDailyScreen(const WeatherData &weather, const char* dateStr);

  // Hit-test helpers for the touchscreen. Callers must pass touch
  // coordinates already converted into PORTRAIT space (see
  // weather_station.ino's handleTouch(), which converts the raw
  // touch-controller reading before calling these).
  bool isDailyPageBackButtonTap(int32_t px, int32_t py);
  bool isHomeScreenWeatherTap(int32_t px, int32_t py);
}
