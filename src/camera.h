#pragma once

#include <Arduino.h>

// Webcam snapshots, decoded in a background task straight from the HTTP
// stream (the printer's camera serves ~250 KB 1080p JPEGs — far more than
// fits in RAM here, so they're never buffered whole).
constexpr int CAM_W = 240;
constexpr int CAM_H = 135;

// Frame buffer (RGB565, room for CAM_W x CAM_H), allocated on first use;
// nullptr if that allocation failed. It holds a frame of the current view
// size, row after row.
uint16_t *camera_framebuffer();

// Size frames are rendered at (at most CAM_W x CAM_H). The UI shows the
// buffer at exactly this size: LVGL never has to scale it, which is slow and
// allocates scratch memory on every redraw (a failed allocation there
// crashed the board).
void camera_set_view(int w, int h);

void camera_set_host(const String &host);   // same "ip[:port]" as moonraker
void camera_set_active(bool active);        // only fetches while the dashboard or cam screen is open

// True if a frame from the current host arrived in the last 20 s.
bool camera_has_feed();

// The UI calls this once frame number f is on screen (or not shown at
// all); the camera waits for it before overwriting the buffer.
void camera_frame_shown(uint32_t f);

// Bumped after each completed frame; the UI compares this to know when to redraw.
uint32_t camera_frame_count();
// Short human-readable status ("" when frames are arriving fine).
void camera_status(char *buf, size_t n);
