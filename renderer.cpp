#include "renderer.h"
#include "display.h"
#include "weather.h"
#include "bus.h"
#include "config.h"
#include "epd_driver.h"
#include "icons.h"          // pre-rotated icon bitmaps + big-digit font
#include "bus_icons.h"      // pre-rotated bus icon -- SEPARATE file from icons.h, see its own
                             // header comment for why (icons.h's original generator is gone)
#include "portrait_font.h"  // pre-rotated alphanumeric UI fonts (regular + small)
#include <Arduino.h>        // Serial -- not pulled in transitively here the way it is in
                             // weather.cpp/weather_ec.cpp/weather_swob.cpp (via WiFi.h)
#include <stdio.h>
#include <stdlib.h>         // malloc/free -- see portraitFillRect()'s NULL-fb fallback
#include <string.h>

// ---- Core portrait placement primitive (verified against a ground-truth
// whole-canvas rotation, and re-verified end-to-end with real text/icons
// before any of this was wired up for real).
static const int32_t PORTRAIT_W = 540; // = EPD_HEIGHT
static const int32_t PORTRAIT_H = 960; // = EPD_WIDTH

static void drawPortraitBitmap(int32_t px, int32_t py, int32_t rotatedW, int32_t rotatedH,
                                const uint8_t* rotatedData, uint8_t* fb) {
  int32_t nativeX = py;
  int32_t nativeY = (PORTRAIT_W - 1) - px - rotatedH + 1;
  Rect_t area = { nativeX, nativeY, rotatedW, rotatedH };
  if (fb) {
    epd_copy_to_framebuffer(area, (uint8_t*)rotatedData, fb);
  } else {
    epd_draw_grayscale_image(area, (uint8_t*)rotatedData);
  }
}

static Rect_t portraitRectToNative(int32_t px0, int32_t py0, int32_t px1, int32_t py1) {
  int32_t nx = py0;
  int32_t ny = (PORTRAIT_W - 1) - px1;
  return { nx, ny, py1 - py0, px1 - px0 };
}

// BUG FIX (see CLAUDE.md's "Known Bugs Fixed" -- this one caused the whole
// panel to appear to full-refresh on every weather change): epd_fill_rect()
// -- and epd_draw_vline()/epd_draw_pixel() underneath it -- unconditionally
// dereference `framebuffer` with no NULL check at all (confirmed by reading
// epd_driver.c directly), unlike epd_draw_grayscale_image() which draws
// straight to the panel and needs no framebuffer pointer. drawPortraitBitmap()
// already accounts for this (fb ? copy-into-framebuffer : draw-direct-to-
// hardware) but this function used to call epd_fill_rect(..., fb) even when
// fb was NULL -- a guaranteed crash/reboot on every partial-update call site
// that passes NULL (i.e. every weather-changed redraw, since
// drawWeatherPartial() draws with fb=NULL by design). Fixed the same way
// drawPortraitBitmap() handles it: build a tiny solid-color scratch buffer
// and push THAT directly to the panel instead of writing into a null
// framebuffer.
static void portraitFillRect(int32_t px0, int32_t py0, int32_t px1, int32_t py1, uint8_t* fb) {
  Rect_t r = portraitRectToNative(px0, py0, px1, py1);
  if (fb) {
    epd_fill_rect(r.x, r.y, r.width, r.height, 0, fb);
    return;
  }
  // 4bpp, 2px/byte, byte value 0x00 = both nibbles = 0 = black (matches this
  // project's own asset-packing convention -- see CLAUDE.md's Asset
  // Generation Pipeline section -- and epd_fill_rect's own color=0 meaning
  // black, so this is a faithful direct-to-hardware equivalent of the
  // fb-based fill above, not an approximation).
  size_t rowBytes = (size_t)(r.width + 1) / 2;
  size_t bytes = rowBytes * (size_t)r.height;
  uint8_t* scratch = (uint8_t*)malloc(bytes);
  if (!scratch) {
    Serial.println("[renderer] portraitFillRect: scratch alloc failed, skipping this rect");
    return; // skip rather than risk another NULL-framebuffer-style crash
  }
  memset(scratch, 0x00, bytes);
  epd_draw_grayscale_image(r, scratch);
  free(scratch);
}

static void toUpperInPlace(char* s) {
  for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
}

// ---- UI text (alphanumeric) glyphs -- regular (34px), small (20px), and
// medium (50px, used for the time so it reads larger than the date) -------
struct PortraitGlyph { const uint8_t* data; int32_t rw, rh, ow; };

#define GLYPH_CASES(PFX) \
    case 'A': return { PFX##_UA_data, (int32_t)PFX##_UA_width, (int32_t)PFX##_UA_height, (int32_t)PFX##_UA_owidth }; \
    case 'B': return { PFX##_UB_data, (int32_t)PFX##_UB_width, (int32_t)PFX##_UB_height, (int32_t)PFX##_UB_owidth }; \
    case 'C': return { PFX##_UC_data, (int32_t)PFX##_UC_width, (int32_t)PFX##_UC_height, (int32_t)PFX##_UC_owidth }; \
    case 'D': return { PFX##_UD_data, (int32_t)PFX##_UD_width, (int32_t)PFX##_UD_height, (int32_t)PFX##_UD_owidth }; \
    case 'E': return { PFX##_UE_data, (int32_t)PFX##_UE_width, (int32_t)PFX##_UE_height, (int32_t)PFX##_UE_owidth }; \
    case 'F': return { PFX##_UF_data, (int32_t)PFX##_UF_width, (int32_t)PFX##_UF_height, (int32_t)PFX##_UF_owidth }; \
    case 'G': return { PFX##_UG_data, (int32_t)PFX##_UG_width, (int32_t)PFX##_UG_height, (int32_t)PFX##_UG_owidth }; \
    case 'H': return { PFX##_UH_data, (int32_t)PFX##_UH_width, (int32_t)PFX##_UH_height, (int32_t)PFX##_UH_owidth }; \
    case 'I': return { PFX##_UI_data, (int32_t)PFX##_UI_width, (int32_t)PFX##_UI_height, (int32_t)PFX##_UI_owidth }; \
    case 'J': return { PFX##_UJ_data, (int32_t)PFX##_UJ_width, (int32_t)PFX##_UJ_height, (int32_t)PFX##_UJ_owidth }; \
    case 'K': return { PFX##_UK_data, (int32_t)PFX##_UK_width, (int32_t)PFX##_UK_height, (int32_t)PFX##_UK_owidth }; \
    case 'L': return { PFX##_UL_data, (int32_t)PFX##_UL_width, (int32_t)PFX##_UL_height, (int32_t)PFX##_UL_owidth }; \
    case 'M': return { PFX##_UM_data, (int32_t)PFX##_UM_width, (int32_t)PFX##_UM_height, (int32_t)PFX##_UM_owidth }; \
    case 'N': return { PFX##_UN_data, (int32_t)PFX##_UN_width, (int32_t)PFX##_UN_height, (int32_t)PFX##_UN_owidth }; \
    case 'O': return { PFX##_UO_data, (int32_t)PFX##_UO_width, (int32_t)PFX##_UO_height, (int32_t)PFX##_UO_owidth }; \
    case 'P': return { PFX##_UP_data, (int32_t)PFX##_UP_width, (int32_t)PFX##_UP_height, (int32_t)PFX##_UP_owidth }; \
    case 'Q': return { PFX##_UQ_data, (int32_t)PFX##_UQ_width, (int32_t)PFX##_UQ_height, (int32_t)PFX##_UQ_owidth }; \
    case 'R': return { PFX##_UR_data, (int32_t)PFX##_UR_width, (int32_t)PFX##_UR_height, (int32_t)PFX##_UR_owidth }; \
    case 'S': return { PFX##_US_data, (int32_t)PFX##_US_width, (int32_t)PFX##_US_height, (int32_t)PFX##_US_owidth }; \
    case 'T': return { PFX##_UT_data, (int32_t)PFX##_UT_width, (int32_t)PFX##_UT_height, (int32_t)PFX##_UT_owidth }; \
    case 'U': return { PFX##_UU_data, (int32_t)PFX##_UU_width, (int32_t)PFX##_UU_height, (int32_t)PFX##_UU_owidth }; \
    case 'V': return { PFX##_UV_data, (int32_t)PFX##_UV_width, (int32_t)PFX##_UV_height, (int32_t)PFX##_UV_owidth }; \
    case 'W': return { PFX##_UW_data, (int32_t)PFX##_UW_width, (int32_t)PFX##_UW_height, (int32_t)PFX##_UW_owidth }; \
    case 'X': return { PFX##_UX_data, (int32_t)PFX##_UX_width, (int32_t)PFX##_UX_height, (int32_t)PFX##_UX_owidth }; \
    case 'Y': return { PFX##_UY_data, (int32_t)PFX##_UY_width, (int32_t)PFX##_UY_height, (int32_t)PFX##_UY_owidth }; \
    case 'Z': return { PFX##_UZ_data, (int32_t)PFX##_UZ_width, (int32_t)PFX##_UZ_height, (int32_t)PFX##_UZ_owidth }; \
    case 'a': return { PFX##_La_data, (int32_t)PFX##_La_width, (int32_t)PFX##_La_height, (int32_t)PFX##_La_owidth }; \
    case 'b': return { PFX##_Lb_data, (int32_t)PFX##_Lb_width, (int32_t)PFX##_Lb_height, (int32_t)PFX##_Lb_owidth }; \
    case 'c': return { PFX##_Lc_data, (int32_t)PFX##_Lc_width, (int32_t)PFX##_Lc_height, (int32_t)PFX##_Lc_owidth }; \
    case 'd': return { PFX##_Ld_data, (int32_t)PFX##_Ld_width, (int32_t)PFX##_Ld_height, (int32_t)PFX##_Ld_owidth }; \
    case 'e': return { PFX##_Le_data, (int32_t)PFX##_Le_width, (int32_t)PFX##_Le_height, (int32_t)PFX##_Le_owidth }; \
    case 'f': return { PFX##_Lf_data, (int32_t)PFX##_Lf_width, (int32_t)PFX##_Lf_height, (int32_t)PFX##_Lf_owidth }; \
    case 'g': return { PFX##_Lg_data, (int32_t)PFX##_Lg_width, (int32_t)PFX##_Lg_height, (int32_t)PFX##_Lg_owidth }; \
    case 'h': return { PFX##_Lh_data, (int32_t)PFX##_Lh_width, (int32_t)PFX##_Lh_height, (int32_t)PFX##_Lh_owidth }; \
    case 'i': return { PFX##_Li_data, (int32_t)PFX##_Li_width, (int32_t)PFX##_Li_height, (int32_t)PFX##_Li_owidth }; \
    case 'j': return { PFX##_Lj_data, (int32_t)PFX##_Lj_width, (int32_t)PFX##_Lj_height, (int32_t)PFX##_Lj_owidth }; \
    case 'k': return { PFX##_Lk_data, (int32_t)PFX##_Lk_width, (int32_t)PFX##_Lk_height, (int32_t)PFX##_Lk_owidth }; \
    case 'l': return { PFX##_Ll_data, (int32_t)PFX##_Ll_width, (int32_t)PFX##_Ll_height, (int32_t)PFX##_Ll_owidth }; \
    case 'm': return { PFX##_Lm_data, (int32_t)PFX##_Lm_width, (int32_t)PFX##_Lm_height, (int32_t)PFX##_Lm_owidth }; \
    case 'n': return { PFX##_Ln_data, (int32_t)PFX##_Ln_width, (int32_t)PFX##_Ln_height, (int32_t)PFX##_Ln_owidth }; \
    case 'o': return { PFX##_Lo_data, (int32_t)PFX##_Lo_width, (int32_t)PFX##_Lo_height, (int32_t)PFX##_Lo_owidth }; \
    case 'p': return { PFX##_Lp_data, (int32_t)PFX##_Lp_width, (int32_t)PFX##_Lp_height, (int32_t)PFX##_Lp_owidth }; \
    case 'q': return { PFX##_Lq_data, (int32_t)PFX##_Lq_width, (int32_t)PFX##_Lq_height, (int32_t)PFX##_Lq_owidth }; \
    case 'r': return { PFX##_Lr_data, (int32_t)PFX##_Lr_width, (int32_t)PFX##_Lr_height, (int32_t)PFX##_Lr_owidth }; \
    case 's': return { PFX##_Ls_data, (int32_t)PFX##_Ls_width, (int32_t)PFX##_Ls_height, (int32_t)PFX##_Ls_owidth }; \
    case 't': return { PFX##_Lt_data, (int32_t)PFX##_Lt_width, (int32_t)PFX##_Lt_height, (int32_t)PFX##_Lt_owidth }; \
    case 'u': return { PFX##_Lu_data, (int32_t)PFX##_Lu_width, (int32_t)PFX##_Lu_height, (int32_t)PFX##_Lu_owidth }; \
    case 'v': return { PFX##_Lv_data, (int32_t)PFX##_Lv_width, (int32_t)PFX##_Lv_height, (int32_t)PFX##_Lv_owidth }; \
    case 'w': return { PFX##_Lw_data, (int32_t)PFX##_Lw_width, (int32_t)PFX##_Lw_height, (int32_t)PFX##_Lw_owidth }; \
    case 'x': return { PFX##_Lx_data, (int32_t)PFX##_Lx_width, (int32_t)PFX##_Lx_height, (int32_t)PFX##_Lx_owidth }; \
    case 'y': return { PFX##_Ly_data, (int32_t)PFX##_Ly_width, (int32_t)PFX##_Ly_height, (int32_t)PFX##_Ly_owidth }; \
    case 'z': return { PFX##_Lz_data, (int32_t)PFX##_Lz_width, (int32_t)PFX##_Lz_height, (int32_t)PFX##_Lz_owidth }; \
    case '0': return { PFX##_D0_data, (int32_t)PFX##_D0_width, (int32_t)PFX##_D0_height, (int32_t)PFX##_D0_owidth }; \
    case '1': return { PFX##_D1_data, (int32_t)PFX##_D1_width, (int32_t)PFX##_D1_height, (int32_t)PFX##_D1_owidth }; \
    case '2': return { PFX##_D2_data, (int32_t)PFX##_D2_width, (int32_t)PFX##_D2_height, (int32_t)PFX##_D2_owidth }; \
    case '3': return { PFX##_D3_data, (int32_t)PFX##_D3_width, (int32_t)PFX##_D3_height, (int32_t)PFX##_D3_owidth }; \
    case '4': return { PFX##_D4_data, (int32_t)PFX##_D4_width, (int32_t)PFX##_D4_height, (int32_t)PFX##_D4_owidth }; \
    case '5': return { PFX##_D5_data, (int32_t)PFX##_D5_width, (int32_t)PFX##_D5_height, (int32_t)PFX##_D5_owidth }; \
    case '6': return { PFX##_D6_data, (int32_t)PFX##_D6_width, (int32_t)PFX##_D6_height, (int32_t)PFX##_D6_owidth }; \
    case '7': return { PFX##_D7_data, (int32_t)PFX##_D7_width, (int32_t)PFX##_D7_height, (int32_t)PFX##_D7_owidth }; \
    case '8': return { PFX##_D8_data, (int32_t)PFX##_D8_width, (int32_t)PFX##_D8_height, (int32_t)PFX##_D8_owidth }; \
    case '9': return { PFX##_D9_data, (int32_t)PFX##_D9_width, (int32_t)PFX##_D9_height, (int32_t)PFX##_D9_owidth }; \
    case ',': return { PFX##_comma_data, (int32_t)PFX##_comma_width, (int32_t)PFX##_comma_height, (int32_t)PFX##_comma_owidth }; \
    case '.': return { PFX##_period_data, (int32_t)PFX##_period_width, (int32_t)PFX##_period_height, (int32_t)PFX##_period_owidth }; \
    case ':': return { PFX##_colon_data, (int32_t)PFX##_colon_width, (int32_t)PFX##_colon_height, (int32_t)PFX##_colon_owidth }; \
    case '%': return { PFX##_percent_data, (int32_t)PFX##_percent_width, (int32_t)PFX##_percent_height, (int32_t)PFX##_percent_owidth }; \
    case '-': return { PFX##_dash_data, (int32_t)PFX##_dash_width, (int32_t)PFX##_dash_height, (int32_t)PFX##_dash_owidth }; \
    case '/': return { PFX##_slash_data, (int32_t)PFX##_slash_width, (int32_t)PFX##_slash_height, (int32_t)PFX##_slash_owidth }; \
    case '\'': return { PFX##_apos_data, (int32_t)PFX##_apos_width, (int32_t)PFX##_apos_height, (int32_t)PFX##_apos_owidth }; \
    case '<': return { PFX##_lt_data, (int32_t)PFX##_lt_width, (int32_t)PFX##_lt_height, (int32_t)PFX##_lt_owidth };

static PortraitGlyph uiGlyphFor(char c) {
  switch (c) {
    case ' ': return { NULL, 0, 0, (int32_t)(ui_font_height * 0.30f) };
    GLYPH_CASES(glyph)
    default: return { NULL, 0, 0, 12 };
  }
}

static PortraitGlyph smallGlyphFor(char c) {
  switch (c) {
    case ' ': return { NULL, 0, 0, (int32_t)(ui_font_small_height * 0.30f) };
    GLYPH_CASES(sglyph)
    default: return { NULL, 0, 0, 8 };
  }
}

// Larger than the regular (34px) body font — used for the time, so it reads
// as more prominent than the date above it.
static PortraitGlyph mediumGlyphFor(char c) {
  switch (c) {
    case ' ': return { NULL, 0, 0, (int32_t)(ui_font_medium_height * 0.30f) };
    GLYPH_CASES(mglyph)
    default: return { NULL, 0, 0, 16 };
  }
}

static int32_t drawPortraitText(int32_t px, int32_t py, const char* text, uint8_t* fb) {
  for (const char* p = text; *p; p++) {
    PortraitGlyph g = uiGlyphFor(*p);
    if (g.data) {
      drawPortraitBitmap(px, py, g.rw, g.rh, g.data, fb);
    }
    px += g.ow + 3;
  }
  return px;
}

static int32_t drawPortraitTextSmall(int32_t px, int32_t py, const char* text, uint8_t* fb) {
  for (const char* p = text; *p; p++) {
    PortraitGlyph g = smallGlyphFor(*p);
    if (g.data) {
      drawPortraitBitmap(px, py, g.rw, g.rh, g.data, fb);
    }
    px += g.ow + 2;
  }
  return px;
}

// Draws exactly one character at a fixed px position, using the MEDIUM
// (50px) font — used by the time's per-slot diffing, where every slot has a
// pre-reserved fixed width regardless of proportional glyph width.
static void drawOneGlyph(int32_t px, int32_t py, char c, uint8_t* fb) {
  PortraitGlyph g = mediumGlyphFor(c);
  if (g.data) drawPortraitBitmap(px, py, g.rw, g.rh, g.data, fb);
}

// ---- Weather condition icons (day + night) --------------------------------
struct IconRef { const uint8_t* data; int32_t rw, rh, ow, oh; };

static IconRef iconForWeather(int code, bool isDay) {
  switch (code) {
    case 0: case 1:
      return isDay ? IconRef{ icon_sun_data, (int32_t)icon_sun_width, (int32_t)icon_sun_height, (int32_t)icon_sun_owidth, (int32_t)icon_sun_oheight }
                   : IconRef{ icon_moon_data, (int32_t)icon_moon_width, (int32_t)icon_moon_height, (int32_t)icon_moon_owidth, (int32_t)icon_moon_oheight };
    case 2:
      return isDay ? IconRef{ icon_partly_cloudy_data, (int32_t)icon_partly_cloudy_width, (int32_t)icon_partly_cloudy_height, (int32_t)icon_partly_cloudy_owidth, (int32_t)icon_partly_cloudy_oheight }
                   : IconRef{ icon_partly_cloudy_night_data, (int32_t)icon_partly_cloudy_night_width, (int32_t)icon_partly_cloudy_night_height, (int32_t)icon_partly_cloudy_night_owidth, (int32_t)icon_partly_cloudy_night_oheight };
    case 3: case 45: case 48:
      return { icon_cloud_data, (int32_t)icon_cloud_width, (int32_t)icon_cloud_height, (int32_t)icon_cloud_owidth, (int32_t)icon_cloud_oheight };
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 65: case 66: case 67:
    case 80: case 81: case 82:
      return { icon_rain_data, (int32_t)icon_rain_width, (int32_t)icon_rain_height, (int32_t)icon_rain_owidth, (int32_t)icon_rain_oheight };
    case 71: case 73: case 75: case 77: case 85: case 86:
      return { icon_snow_data, (int32_t)icon_snow_width, (int32_t)icon_snow_height, (int32_t)icon_snow_owidth, (int32_t)icon_snow_oheight };
    case 95: case 96: case 99:
      return { icon_storm_data, (int32_t)icon_storm_width, (int32_t)icon_storm_height, (int32_t)icon_storm_owidth, (int32_t)icon_storm_oheight };
    default:
      return { icon_cloud_data, (int32_t)icon_cloud_width, (int32_t)icon_cloud_height, (int32_t)icon_cloud_owidth, (int32_t)icon_cloud_oheight };
  }
}

// ---- Big-digit font for the temperature readout ---------------------------
static IconRef bigGlyphFor(char c) {
  switch (c) {
    case '0': return { bigfont_d0_data, (int32_t)bigfont_d0_width, (int32_t)bigfont_d0_height, (int32_t)bigfont_d0_owidth, (int32_t)bigfont_oheight };
    case '1': return { bigfont_d1_data, (int32_t)bigfont_d1_width, (int32_t)bigfont_d1_height, (int32_t)bigfont_d1_owidth, (int32_t)bigfont_oheight };
    case '2': return { bigfont_d2_data, (int32_t)bigfont_d2_width, (int32_t)bigfont_d2_height, (int32_t)bigfont_d2_owidth, (int32_t)bigfont_oheight };
    case '3': return { bigfont_d3_data, (int32_t)bigfont_d3_width, (int32_t)bigfont_d3_height, (int32_t)bigfont_d3_owidth, (int32_t)bigfont_oheight };
    case '4': return { bigfont_d4_data, (int32_t)bigfont_d4_width, (int32_t)bigfont_d4_height, (int32_t)bigfont_d4_owidth, (int32_t)bigfont_oheight };
    case '5': return { bigfont_d5_data, (int32_t)bigfont_d5_width, (int32_t)bigfont_d5_height, (int32_t)bigfont_d5_owidth, (int32_t)bigfont_oheight };
    case '6': return { bigfont_d6_data, (int32_t)bigfont_d6_width, (int32_t)bigfont_d6_height, (int32_t)bigfont_d6_owidth, (int32_t)bigfont_oheight };
    case '7': return { bigfont_d7_data, (int32_t)bigfont_d7_width, (int32_t)bigfont_d7_height, (int32_t)bigfont_d7_owidth, (int32_t)bigfont_oheight };
    case '8': return { bigfont_d8_data, (int32_t)bigfont_d8_width, (int32_t)bigfont_d8_height, (int32_t)bigfont_d8_owidth, (int32_t)bigfont_oheight };
    case '9': return { bigfont_d9_data, (int32_t)bigfont_d9_width, (int32_t)bigfont_d9_height, (int32_t)bigfont_d9_owidth, (int32_t)bigfont_oheight };
    case '.': return { bigfont_dot_data, (int32_t)bigfont_dot_width, (int32_t)bigfont_dot_height, (int32_t)bigfont_dot_owidth, (int32_t)bigfont_oheight };
    case '-': return { bigfont_dash_data, (int32_t)bigfont_dash_width, (int32_t)bigfont_dash_height, (int32_t)bigfont_dash_owidth, (int32_t)bigfont_oheight };
    case 'C': return { bigfont_letterC_data, (int32_t)bigfont_letterC_width, (int32_t)bigfont_letterC_height, (int32_t)bigfont_letterC_owidth, (int32_t)bigfont_oheight };
    default:  return { NULL, 0, 0, 0, 0 };
  }
}

static int32_t drawBigNumber(int32_t px, int32_t py, const char* text, uint8_t* fb) {
  for (const char* p = text; *p; p++) {
    IconRef g = bigGlyphFor(*p);
    if (!g.data) continue;
    drawPortraitBitmap(px, py, g.rw, g.rh, g.data, fb);
    px += g.ow + 6;
  }
  return px;
}

// ---- Layout constants (portrait: px 0..539 horizontal, py 0..959 vertical)
// Header: location name (one line, pin icon), date below it, then the time
// (larger than the date, no box around it) below that. Main: condition icon
// + big temp + condition/feels-like. Footer: 2-column grid (Humidity | Wind).
// "Last updated" moved out of the footer grid entirely and shown small,
// bottom-left, de-emphasized.
static const int32_t MARGIN        = 20;
static const int32_t LOCATION_PY   = 20;
// Date pulled a bit closer to the location line above it (was 70 -- a 16px
// gap below the location row's ~34px-tall content; now an ~8px gap), and
// TIME_TEXT_Y pushed down accordingly (was 115) to give the date/time pair
// more real separation rather than less. This also happens to fix the
// date-descender-fade issue in "Known Open Issues" (CLAUDE.md) as a side
// effect: DATE_PY1/TIME_PY0 still share the same formula (TIME_TEXT_Y-5),
// but there's now ~25-30px of genuine clearance between where date ink
// actually ends and that shared clear boundary, comfortably more than any
// descender's depth -- unlike the reverted first attempt, this doesn't
// narrow TIME_PY0's own coverage of the time digits' real ink at all.
static const int32_t DATE_PY       = 62;
static const int32_t TIME_TEXT_X   = MARGIN;
static const int32_t TIME_TEXT_Y   = 130;
static const int32_t HEADER_DIV_PY = TIME_TEXT_Y + (int32_t)ui_font_medium_height + 20;
static const int32_t MAIN_TOP      = HEADER_DIV_PY + 20; // shifts down with TIME_TEXT_Y --
                                                          // this is the "move weather section
                                                          // a little lower" the extra header
                                                          // room was traded for.
// Removed 2026-09-11: the old FOOTER_* constants (a 2-column Humidity |
// Wind grid at y=610-840, with its own divider at 610) — dropped to make
// room for the new "Next Buses" section below, per the layout mockup
// (`new layout.png`) moving humidity to a future dedicated Weather tab
// and condensing feels-like/wind onto the main weather block itself. See
// CLAUDE.md's "Planned UI" section. Humidity is NOT shown anywhere on
// this screen right now as a result — known, deliberate, temporary gap
// until the Weather tab exists.
static const int32_t LASTUPD_PY    = 905;

// --- Bus section (added 2026-09-11 -- see CLAUDE.md's "Planned UI" for
// the full mockup this is a first, deliberately partial step toward: just
// a compact list on the existing Home screen, no tabs/drill-down yet).
// Sits in the space freed by removing the old Humidity|Wind footer above.
static const int32_t BUS_DIV_PY       = 510; // divider between the weather block and this section
static const int32_t BUS_HEADER_PY    = 530; // "NEXT BUSES" label row
static const int32_t BUS_LIST_TOP     = 585; // first configured stop's row
static const int32_t BUS_ROW_HEIGHT   = 95;  // vertical space per stop: label + gap + arrivals + spacing
static const int32_t BUS_ARRIVALS_GAP = 8;   // gap between a stop's label line and its arrivals line
static const int32_t BUS_COL2_PX      = PORTRAIT_W / 2 + 10; // 2nd arrival column start x

// Partial-refresh regions, in PORTRAIT space (converted to native internally).
// Time gets its OWN independent region so a minute-tick refresh never
// touches the date, location, or anything else on the panel.
//
// DATE_PY1 and TIME_PY0 still share the same formula (TIME_TEXT_Y - 5) --
// see CLAUDE.md's "Known Open Issues" for the full history of why that
// was a problem (date-line descenders slowly fading under the per-minute
// clear) and why the first fix attempt (narrowing TIME_PY0's own coverage
// of the digit glyphs) was reverted. The DATE_PY/TIME_TEXT_Y repositioning
// above resolves it a different way -- by giving the date line enough real
// clearance above this shared boundary that no descender should reach it
// -- without touching TIME_PY0's digit coverage at all. Pending
// confirmation on a fresh hardware photo before calling this closed.
static const int32_t DATE_PY0 = DATE_PY - 10, DATE_PY1 = TIME_TEXT_Y - 5;
static const int32_t TIME_PY0 = TIME_TEXT_Y - 5, TIME_PY1 = TIME_TEXT_Y + (int32_t)ui_font_medium_height + 5;
// Weather block's own region (icon/temp/condition/feels-like/wind ONLY --
// does NOT extend down through the bus section or the last-updated line
// any more, now that there's non-weather content physically between them.
// Sharing one wide region across content that refreshes on different
// triggers is exactly the "Known Open Issues" mistake above, just at a
// bigger scale -- each independently-triggered region needs its own
// bounds, not a shared one that happens to cover multiple things.
static const int32_t WEATHER_PY0 = MAIN_TOP - 10, WEATHER_PY1 = BUS_DIV_PY - 5;
static const int32_t LASTUPD_REGION_PY0 = LASTUPD_PY - 5;
static const int32_t LASTUPD_REGION_PY1 = LASTUPD_PY + (int32_t)ui_font_small_height + 10;
// Whole bus section (divider through the last configured stop's row).
// Recompute if BUS_STOP_COUNT (config.h) or BUS_ROW_HEIGHT above change.
static const int32_t BUS_REGION_PY0 = BUS_DIV_PY - 10;
static const int32_t BUS_REGION_PY1 = BUS_LIST_TOP + (int32_t)BUS_STOP_COUNT * BUS_ROW_HEIGHT + 10;

// Daily-page back button, in PORTRAIT space.
static const int32_t BACK_PX0 = 20, BACK_PY0 = 20, BACK_PX1 = 170, BACK_PY1 = 80;

// ---- Time: per-character "slot" layout for cheap digit-only partial
// refresh, drawn in the larger medium (50px) font, no border. The combo
// string is always the fixed shape "HH:MM AP MTZT" (12 chars, e.g.
// "12:49 AM EDT") because %I/%M are zero-padded and this project's
// timezone strings (EST/EDT) are always 3 letters. Each slot has a FIXED
// reserved width, sized to the widest character that can ever appear there
// (e.g. slot 6 is always 'A' or 'P' — reserve max(A,P)) — so redrawing a
// slot never needs to touch its neighbours, whichever character lands there.
static const int TIME_COMBO_LEN = 12;
static const int32_t TIME_SLOT_RAWW[TIME_COMBO_LEN] = {
  28, 28, 18, 28, 28, 15, 34, 38, 15, 32, 34, 30
  // H    H    :   M    M   ' '  A/P  M   ' '   E  S/D   T
};
static const int32_t TIME_SLOT_GAP = 4;

static int32_t timeSlotX(int idx) {
  int32_t x = TIME_TEXT_X;
  for (int i = 0; i < idx; i++) x += TIME_SLOT_RAWW[i] + TIME_SLOT_GAP;
  return x;
}

// Remembers what's actually on the panel right now, so a minute tick can
// diff against it and only touch the characters that changed.
static char lastTimeCombo[TIME_COMBO_LEN + 1] = "";

static void drawStaticChrome(uint8_t* fb) {
  portraitFillRect(MARGIN, HEADER_DIV_PY, PORTRAIT_W - MARGIN, HEADER_DIV_PY + 2, fb);
  // The bus-section divider (BUS_DIV_PY) is intentionally NOT drawn here --
  // drawBusValues() redraws it every time (same reasoning the old footer's
  // inner Humidity|Wind column divider used: it sits inside a region that
  // gets cleared+redrawn as a whole, so it has to be part of that redraw,
  // not "static" chrome drawn once and never touched again).
}

// Location name, one line: pin icon + text (fits comfortably at full width
// now that the header isn't split into two columns).
static void drawLocationName(uint8_t* fb) {
  drawPortraitBitmap(MARGIN, LOCATION_PY, (int32_t)icon_location_width, (int32_t)icon_location_height, icon_location_data, fb);
  int32_t textX = MARGIN + (int32_t)icon_location_oheight + 12;
  drawPortraitText(textX, LOCATION_PY, WEATHER_LOCATION_NAME, fb);
}

static void drawDateValue(const char* dateStr, uint8_t* fb) {
  drawPortraitText(MARGIN, DATE_PY, dateStr, fb);
}

// Full (re)draw of the time: every character at its fixed slot position, no
// border. Used for the initial full-screen draw, and as a fallback if the
// combo string's shape ever doesn't match what per-slot diffing expects
// (defensive — shouldn't happen given this project's fixed TZ).
static void drawTimeValueFull(const char* combo, uint8_t* fb) {
  for (int i = 0; i < TIME_COMBO_LEN && combo[i]; i++) {
    drawOneGlyph(timeSlotX(i), TIME_TEXT_Y, combo[i], fb);
  }
}

// Icon + big temp + condition + feels-like + wind ONLY -- humidity dropped
// (see the comment where FOOTER_* used to be defined) and "last updated"
// split out into its own function/region (drawLastUpdatedLine()) since
// there's non-weather content (the bus section) physically between them
// now, so they can no longer share one partial-refresh region.
static void drawWeatherValues(const WeatherData &weather, uint8_t* fb) {
  char line[64];

  if (!weather.valid) {
    drawPortraitText(MARGIN, MAIN_TOP + 20, "Waiting for first weather", fb);
    drawPortraitText(MARGIN, MAIN_TOP + 60, "update...", fb);
    return;
  }

  // Condition icon + big temperature, side by side
  IconRef condIcon = iconForWeather(weather.weatherCode, weather.isDay);
  drawPortraitBitmap(MARGIN, MAIN_TOP, condIcon.rw, condIcon.rh, condIcon.data, fb);

  int32_t textX = MARGIN + condIcon.oh + 15;
  snprintf(line, sizeof(line), "%.1fC", weather.temperatureC);
  drawBigNumber(textX, MAIN_TOP + 10, line, fb);

  int32_t py = MAIN_TOP + 10 + (int32_t)bigfont_oheight + 15;
  strncpy(line, weather.conditionText, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  toUpperInPlace(line);
  drawPortraitText(textX, py, line, fb);
  py += (int32_t)ui_font_height + 12;

  snprintf(line, sizeof(line), "Feels like %.0fC", weather.feelsLikeC);
  drawPortraitText(textX, py, line, fb);
  py += (int32_t)ui_font_height + 8;

  snprintf(line, sizeof(line), "Wind %.1f km/h %s", weather.windSpeedKph,
           windDirectionToCompass(weather.windDirectionDeg));
  drawPortraitText(textX, py, line, fb);
}

// "Weather updated at ...", small and de-emphasized -- its own function/
// region now (see WEATHER_PY1/LASTUPD_REGION_* above) rather than the tail
// end of drawWeatherValues(), since the bus section sits between them on
// screen and each needs to be clearable independently.
static void drawLastUpdatedLine(const WeatherData &weather, uint8_t* fb) {
  if (!weather.valid) return;
  char updated12h[16], line[48];
  formatTime12h(weather.lastUpdated, updated12h, sizeof(updated12h));
  snprintf(line, sizeof(line), "Weather updated at %s", updated12h);
  drawPortraitTextSmall(MARGIN, LASTUPD_PY, line, fb);
}

// "NEXT BUSES" section: a divider + header, then one row per configured
// stop (BUS_STOP_COUNT entries, config.h) -- label line, then up to 2
// arrivals side by side (route, ETA, delay-if-live, clock time). First
// pass at this layout -- like every other layout constant in this
// project, treat BUS_* above as "best estimate, needs a real hardware
// photo to confirm," not as tuned/final (see CLAUDE.md's "Planned UI").
static void drawBusValues(const BusStopResult* results, int count, uint8_t* fb) {
  portraitFillRect(MARGIN, BUS_DIV_PY, PORTRAIT_W - MARGIN, BUS_DIV_PY + 2, fb);

  drawPortraitBitmap(MARGIN, BUS_HEADER_PY, (int32_t)icon_bus_width, (int32_t)icon_bus_height, icon_bus_data, fb);
  drawPortraitText(MARGIN + (int32_t)icon_bus_oheight + 12, BUS_HEADER_PY, "NEXT BUSES", fb);

  int32_t rowY = BUS_LIST_TOP;
  for (int i = 0; i < count; i++, rowY += BUS_ROW_HEIGHT) {
    const BusStopResult &r = results[i];
    drawPortraitText(MARGIN, rowY, r.label, fb);

    int32_t arrivalsY = rowY + (int32_t)ui_font_height + BUS_ARRIVALS_GAP;
    if (!r.hasAnyData) {
      drawPortraitTextSmall(MARGIN, arrivalsY, "No data", fb);
      continue;
    }
    if (r.arrivalCount == 0) {
      drawPortraitTextSmall(MARGIN, arrivalsY, "No upcoming buses", fb);
      continue;
    }

    int32_t colX[2] = { MARGIN, BUS_COL2_PX };
    for (int c = 0; c < 2 && c < r.arrivalCount; c++) {
      const BusArrival &a = r.arrivals[c];
      char eta[16], clock[16], line[64];
      formatBusEta(a.minutes, a.live, eta, sizeof(eta));
      formatBusClock12h(a.epoch, clock, sizeof(clock));
      if (a.live && a.delayMin != 0) {
        snprintf(line, sizeof(line), "%s %s (%+d) %s", a.route, eta, a.delayMin, clock);
      } else {
        snprintf(line, sizeof(line), "%s %s %s", a.route, eta, clock);
      }
      drawPortraitTextSmall(colX[c], arrivalsY, line, fb);
    }
  }
}

void Renderer::drawFullScreen(const WeatherData &weather, const BusStopResult* busResults, int busCount,
                               const char* timeStr12h, const char* dateStr, const char* tzStr) {
  Display::clearBuffer();
  uint8_t* fb = Display::framebuffer();

  char combo[TIME_COMBO_LEN + 1];
  snprintf(combo, sizeof(combo), "%s %s", timeStr12h, tzStr);

  drawStaticChrome(fb);
  drawLocationName(fb);
  drawDateValue(dateStr, fb);
  drawTimeValueFull(combo, fb);
  drawWeatherValues(weather, fb);
  drawLastUpdatedLine(weather, fb);
  drawBusValues(busResults, busCount, fb);

  Display::fullRefresh();

  strncpy(lastTimeCombo, combo, sizeof(lastTimeCombo) - 1);
  lastTimeCombo[sizeof(lastTimeCombo) - 1] = '\0';
}

// Only the characters that actually changed since the last tick get cleared
// and redrawn — a normal minute tick touches just the last minute digit; a
// ":X9 -> :X0" rollover touches both minute digits; an hour rollover also
// touches the hour digits (and AM/PM, right at noon/midnight). Everything
// else is left completely untouched, so there's no flash there at all.
void Renderer::drawTimePartial(const char* timeStr12h, const char* tzStr) {
  char combo[TIME_COMBO_LEN + 1];
  snprintf(combo, sizeof(combo), "%s %s", timeStr12h, tzStr);

  bool freshStart = (strlen(lastTimeCombo) != (size_t)TIME_COMBO_LEN) ||
                     (strlen(combo) != (size_t)TIME_COMBO_LEN);

  epd_poweron();
  if (freshStart) {
    // First tick this boot (or an unexpected format change) — draw the
    // whole row once so there's a known-good baseline to diff against.
    epd_clear_area(portraitRectToNative(TIME_TEXT_X - 5, TIME_PY0, PORTRAIT_W - MARGIN, TIME_PY1));
    drawTimeValueFull(combo, NULL);
  } else {
    for (int i = 0; i < TIME_COMBO_LEN; i++) {
      if (combo[i] != lastTimeCombo[i]) {
        int32_t slotX = timeSlotX(i);
        epd_clear_area(portraitRectToNative(slotX, TIME_PY0, slotX + TIME_SLOT_RAWW[i] + TIME_SLOT_GAP, TIME_PY1));
        drawOneGlyph(slotX, TIME_TEXT_Y, combo[i], NULL);
      }
    }
  }
  epd_poweroff();

  strncpy(lastTimeCombo, combo, sizeof(lastTimeCombo) - 1);
  lastTimeCombo[sizeof(lastTimeCombo) - 1] = '\0';
}

void Renderer::drawDatePartial(const char* dateStr) {
  epd_poweron();
  epd_clear_area(portraitRectToNative(MARGIN - 5, DATE_PY0, PORTRAIT_W - MARGIN, DATE_PY1));
  drawDateValue(dateStr, NULL);
  epd_poweroff();
}

void Renderer::drawWeatherPartial(const WeatherData &weather) {
  epd_poweron();
  epd_clear_area(portraitRectToNative(MARGIN - 10, WEATHER_PY0, PORTRAIT_W - MARGIN + 10, WEATHER_PY1));
  drawWeatherValues(weather, NULL);
  // Separate region: the bus section now sits physically between the
  // weather block and this line, so they can't share one clear+redraw
  // any more (see WEATHER_PY1/LASTUPD_REGION_* above).
  epd_clear_area(portraitRectToNative(MARGIN - 10, LASTUPD_REGION_PY0, PORTRAIT_W - MARGIN + 10, LASTUPD_REGION_PY1));
  drawLastUpdatedLine(weather, NULL);
  epd_poweroff();
}

void Renderer::drawBusPartial(const BusStopResult* busResults, int busCount) {
  epd_poweron();
  epd_clear_area(portraitRectToNative(MARGIN - 10, BUS_REGION_PY0, PORTRAIT_W - MARGIN + 10, BUS_REGION_PY1));
  drawBusValues(busResults, busCount, NULL);
  epd_poweroff();
}

void Renderer::drawDailyScreen(const WeatherData &weather, const char* dateStr) {
  Display::clearBuffer();
  uint8_t* fb = Display::framebuffer();
  char line[64];

  Rect_t backNative = portraitRectToNative(BACK_PX0, BACK_PY0, BACK_PX1, BACK_PY1);
  epd_draw_rect(backNative.x, backNative.y, backNative.width, backNative.height, 0, fb);
  drawPortraitText(BACK_PX0 + 15, BACK_PY0 + 15, "< Back", fb);

  drawPortraitText(MARGIN, 110, "Today's Overview", fb);
  drawPortraitText(MARGIN, 150, dateStr, fb);
  portraitFillRect(MARGIN, 190, PORTRAIT_W - MARGIN, 192, fb);

  if (!weather.valid) {
    drawPortraitText(MARGIN, 230, "No weather data yet.", fb);
    Display::fullRefresh();
    return;
  }

  int32_t py = 230;
  snprintf(line, sizeof(line), "High: %.1f C", weather.tempMaxC);
  drawPortraitText(MARGIN, py, line, fb);
  py += 50;
  snprintf(line, sizeof(line), "Low: %.1f C", weather.tempMinC);
  drawPortraitText(MARGIN, py, line, fb);
  py += 70;

  char sunrise12h[16], sunset12h[16];
  formatTime12h(weather.sunrise, sunrise12h, sizeof(sunrise12h));
  formatTime12h(weather.sunset, sunset12h, sizeof(sunset12h));

  drawPortraitBitmap(MARGIN, py, (int32_t)icon_sunrise_width, (int32_t)icon_sunrise_height, icon_sunrise_data, fb);
  snprintf(line, sizeof(line), "Sunrise: %s", sunrise12h);
  drawPortraitText(MARGIN + (int32_t)icon_sunrise_oheight + 12, py, line, fb);
  py += (int32_t)icon_sunrise_oheight + 20;

  drawPortraitBitmap(MARGIN, py, (int32_t)icon_sunset_width, (int32_t)icon_sunset_height, icon_sunset_data, fb);
  snprintf(line, sizeof(line), "Sunset: %s", sunset12h);
  drawPortraitText(MARGIN + (int32_t)icon_sunset_oheight + 12, py, line, fb);
  py += (int32_t)icon_sunset_oheight + 30;

  drawPortraitBitmap(MARGIN, py, (int32_t)icon_wind_width, (int32_t)icon_wind_height, icon_wind_data, fb);
  snprintf(line, sizeof(line), "Max wind: %.1f km/h", weather.windMaxKph);
  drawPortraitText(MARGIN + (int32_t)icon_wind_oheight + 12, py, line, fb);
  py += (int32_t)icon_wind_oheight + 30;

  snprintf(line, sizeof(line), "Chance of rain: %d%%", weather.precipProbabilityMax);
  drawPortraitText(MARGIN, py, line, fb);

  Display::fullRefresh();
}

bool Renderer::isDailyPageBackButtonTap(int32_t px, int32_t py) {
  return px >= BACK_PX0 && px < BACK_PX1 && py >= BACK_PY0 && py < BACK_PY1;
}

// Bounds shrunk to just the weather block now that the old footer is gone
// and there's a non-interactive bus section below it instead (no
// drill-down page for bus data exists yet -- see CLAUDE.md's "Planned
// UI" -- so that area intentionally does nothing on tap for now).
bool Renderer::isHomeScreenWeatherTap(int32_t px, int32_t py) {
  return px >= 0 && px < PORTRAIT_W && py >= WEATHER_PY0 && py < WEATHER_PY1;
}
