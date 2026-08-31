#include "display.h"
#include "epd_driver.h"
#include <Arduino.h>
#include <string.h>

static uint8_t* fb = nullptr;

bool Display::init() {
  epd_init();
  fb = (uint8_t*)heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM);
  if (!fb) {
    Serial.println("[display] FATAL: framebuffer allocation failed");
    return false;
  }
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2); // 0xFF = white
  return true;
}

uint8_t* Display::framebuffer() { return fb; }

void Display::clearBuffer() {
  memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
}

void Display::fullRefresh() {
  epd_poweron();
  epd_clear();
  epd_draw_grayscale_image(epd_full_screen(), fb);
  epd_poweroff(); // no reason to leave the HV rail energized between refreshes
}
