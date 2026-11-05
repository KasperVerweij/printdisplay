#pragma once

#include <Arduino.h>

// Lightweight runtime stats, printed as "[perf] ..." lines every 10 s:
// LVGL refresh count and time, the longest loop() stall, heap, camera
// frame times and each task's spare stack.
void perf_init();                      // after display_init()
void perf_loop(uint32_t loop_ms);      // from loop(), with that iteration's time
void perf_camera_frame(uint32_t download_ms, uint32_t decode_ms, uint32_t bytes);
