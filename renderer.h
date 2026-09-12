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
  // e-ink refresh. Call once from setup().
  void drawFullScreen(const WeatherData &weather, const char* timeStr12h,
                       const char* dateStr, const char* tzStr);

  // Partial refresh of ONLY the time+timezone area — does not touch the
  // date or anything else. Call every minute.
  void drawTimePartial(const char* timeStr12h, const char* tzStr);

  // Partial refresh of ONLY the date area — does not touch the time or
  // anything else. Call whenever the date string has actually changed
  // (once a day), not on every minute tick, so it doesn't flash for no
  // reason.
  void drawDatePartial(const char* dateStr);

  // Partial refresh of ONLY the weather info area (temp, wind, condition,
  // feels like, humidity, last update) — does not touch the rest of the
  // panel. Call every 30 minutes (whenever fresh weather data arrives).
  void drawWeatherPartial(const WeatherData &weather);

  // Full-screen "today at a glance" page: high/low, sunrise/sunset, max
  // wind, precipitation chance, plus a back button. Full refresh — this is
  // a totally different layout from the home screen, not a partial update.
  void drawDailyScreen(const WeatherData &weather, const char* dateStr);

  // FIRST, deliberately minimal step toward showing bus data (see
  // CLAUDE.md's "Planned UI" / "Known Bugs Fixed" for why this started
  // small after the full module's static-schedule path crashed on real
  // hardware): draws ONE line of plain text just under the weather block,
  // e.g. "Next bus (11): 5 min". Its own independent partial-refresh
  // region — does not touch anything drawWeatherPartial() etc. already
  // cover. Pass "" (empty string) to clear the line back to blank (still
  // does the clear, so a "no data now" case doesn't leave stale text).
  void drawNextBusLine(const char* text);

  // Hit-test helpers for the touchscreen. Callers must pass touch
  // coordinates already converted into PORTRAIT space (see
  // weather_station.ino's handleTouch(), which converts the raw
  // touch-controller reading before calling these).
  bool isDailyPageBackButtonTap(int32_t px, int32_t py);
  bool isHomeScreenWeatherTap(int32_t px, int32_t py);
}
