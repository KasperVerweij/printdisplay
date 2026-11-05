#pragma once

// Builds all LVGL screens (Wi-Fi picker, text entry keyboard, connecting,
// printer dashboard, settings) and shows the right one to start
// (auto-reconnect with saved credentials, or the picker if there are none).
// Call once after display_init().
void ui_init();

// Debug over serial (see main.cpp): d = dashboard, c = camera, g = settings,
// p = printer list, s = screenshot, l = print the printer list, 1-9 = pick
// that printer from it. Menu: m = menu, t = temperatures, v = move,
// u = tune, k = macros, f = files, w = power.
void ui_debug_command(char c);
