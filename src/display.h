#pragma once

// Landscape: this board's panel is physically wider than tall.
constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;

// Initializes TFT_eSPI + LVGL (display flush + touch input driver) and runs
// a one-time interactive touch calibration on first boot (persisted in NVS
// via Preferences, so it only happens once per board). Call before any
// other LVGL calls.
void display_init();

// Debug: sends the next full redraw of the screen over serial as
// "[shot] x y w h" + base64 RGB565 lines, ending with "[shot] end".
// tools/screenshot.py turns that into a PNG.
void display_request_screenshot();

