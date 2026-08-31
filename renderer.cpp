#include "renderer.h"
#include "display.h"
#include "weather.h"
#include "config.h"
#include "epd_driver.h"
#include "icons.h"          // pre-rotated icon bitmaps + big-digit font
#include "portrait_font.h"  // pre-rotated alphanumeric UI fonts (regular + small)
#include <stdio.h>
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

static void portraitFillRect(int32_t px0, int32_t py0, int32_t px1, int32_t py1, uint8_t* fb) {
  Rect_t r = portraitRectToNative(px0, py0, px1, py1);
  epd_fill_rect(r.x, r.y, r.width, r.height, 0, fb);
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
static const int32_t DATE_PY       = 70;
static const int32_t TIME_TEXT_X   = MARGIN;
static const int32_t TIME_TEXT_Y   = 115;
static const int32_t HEADER_DIV_PY = TIME_TEXT_Y + (int32_t)ui_font_medium_height + 20;
static const int32_t MAIN_TOP      = HEADER_DIV_PY + 20;
static const int32_t FOOTER_DIV_PY = 610;
static const int32_t FOOTER_TOP    = 630;
static const int32_t FOOTER_COL_DIV_PX = 270; // splits Humidity | Wind
static const int32_t FOOTER_BOTTOM_PY  = 840;
static const int32_t LASTUPD_PY    = 905;

// Partial-refresh regions, in PORTRAIT space (converted to native internally).
// Time gets its OWN independent region so a minute-tick refresh never
// touches the date, location, or anything else on the panel.
static const int32_t DATE_PY0 = LOCATION_PY + 40, DATE_PY1 = TIME_TEXT_Y - 5;
static const int32_t TIME_PY0 = TIME_TEXT_Y - 5, TIME_PY1 = TIME_TEXT_Y + (int32_t)ui_font_medium_height + 5;
static const int32_t WEATHER_PY0 = MAIN_TOP - 10, WEATHER_PY1 = LASTUPD_PY + 30;

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
  portraitFillRect(MARGIN, FOOTER_DIV_PY, PORTRAIT_W - MARGIN, FOOTER_DIV_PY + 2, fb);
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

static void drawWeatherValues(const WeatherData &weather, uint8_t* fb) {
  char line[64];

  if (!weather.valid) {
    drawPortraitText(MARGIN, MAIN_TOP + 20, "Waiting for first weather", fb);
    drawPortraitText(MARGIN, MAIN_TOP + 60, "update...", fb);
    return;
  }

  // Redraw the footer's vertical divider each time (this region gets
  // cleared and redrawn as a whole on every weather update).
  portraitFillRect(FOOTER_COL_DIV_PX, FOOTER_TOP, FOOTER_COL_DIV_PX + 2, FOOTER_BOTTOM_PY - 10, fb);

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

  // Footer: Humidity | Wind, 2 columns
  drawPortraitBitmap(MARGIN, FOOTER_TOP, (int32_t)icon_humidity_width, (int32_t)icon_humidity_height, icon_humidity_data, fb);
  drawPortraitText(MARGIN, FOOTER_TOP + (int32_t)icon_humidity_oheight + 15, "HUMIDITY:", fb);
  snprintf(line, sizeof(line), "%d%%", weather.humidityPct);
  drawPortraitText(MARGIN, FOOTER_TOP + (int32_t)icon_humidity_oheight + 50, line, fb);

  int32_t col2X = FOOTER_COL_DIV_PX + 20;
  drawPortraitBitmap(col2X, FOOTER_TOP, (int32_t)icon_wind_width, (int32_t)icon_wind_height, icon_wind_data, fb);
  drawPortraitText(col2X, FOOTER_TOP + (int32_t)icon_wind_oheight + 15, "WIND:", fb);
  snprintf(line, sizeof(line), "%.1f km/h", weather.windSpeedKph);
  drawPortraitText(col2X, FOOTER_TOP + (int32_t)icon_wind_oheight + 50, line, fb);
  drawPortraitText(col2X, FOOTER_TOP + (int32_t)icon_wind_oheight + 85, windDirectionToCompass(weather.windDirectionDeg), fb);

  // "Last updated", small and de-emphasized, bottom-left.
  char updated12h[16];
  formatTime12h(weather.lastUpdated, updated12h, sizeof(updated12h));
  snprintf(line, sizeof(line), "Weather updated at %s", updated12h);
  drawPortraitTextSmall(MARGIN, LASTUPD_PY, line, fb);
}

void Renderer::drawFullScreen(const WeatherData &weather, const char* timeStr12h,
                               const char* dateStr, const char* tzStr) {
  Display::clearBuffer();
  uint8_t* fb = Display::framebuffer();

  char combo[TIME_COMBO_LEN + 1];
  snprintf(combo, sizeof(combo), "%s %s", timeStr12h, tzStr);

  drawStaticChrome(fb);
  drawLocationName(fb);
  drawDateValue(dateStr, fb);
  drawTimeValueFull(combo, fb);
  drawWeatherValues(weather, fb);

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

bool Renderer::isHomeScreenWeatherTap(int32_t px, int32_t py) {
  return px >= 0 && px < PORTRAIT_W && py >= WEATHER_PY0 && py < FOOTER_DIV_PY;
}
