#pragma once
#include <stdint.h>

// Thin wrapper around the LilyGo EPD47 driver — the only file that
// includes epd_driver.h directly.
namespace Display {
  // Call once from setup(). Allocates the framebuffer in PSRAM.
  // Returns false if allocation fails (should not happen on this board
  // with OPI PSRAM enabled correctly — if it does, check Tools > PSRAM).
  bool init();

  uint8_t* framebuffer();   // for renderer.cpp to draw into
  void clearBuffer();       // fills framebuffer white, no panel update
  void fullRefresh();       // pushes framebuffer to panel (full refresh)
}
