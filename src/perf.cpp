#include "perf.h"

#include <lvgl.h>

namespace {

constexpr uint32_t REPORT_MS = 10000;

uint32_t refr_start_us = 0;
uint32_t refr_count = 0, refr_total_us = 0, refr_max_us = 0;
uint32_t loop_max_ms = 0;
uint32_t last_report = 0;

portMUX_TYPE cam_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t cam_frames = 0, cam_dl_ms = 0, cam_dec_ms = 0, cam_bytes = 0;

void on_refr(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        refr_start_us = micros();
    } else {
        uint32_t d = micros() - refr_start_us;
        refr_count++;
        refr_total_us += d;
        if (d > refr_max_us) refr_max_us = d;
    }
}

void stack_report(char *buf, size_t n) {
    static const char *names[] = {"loopTask", "moonraker", "estop", "camera", "pscan", "arppin"};
    size_t used = 0;
    buf[0] = 0;
    for (const char *name : names) {
        TaskHandle_t t = xTaskGetHandle(name);
        if (!t) continue;
        used += snprintf(buf + used, n - used, " %s:%u", name, (unsigned)uxTaskGetStackHighWaterMark(t));
        if (used >= n) break;
    }
}

} // namespace

void perf_init() {
    lv_display_t *d = lv_display_get_default();
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_READY, nullptr);
}

void perf_loop(uint32_t loop_ms) {
    if (loop_ms > loop_max_ms) loop_max_ms = loop_ms;
    uint32_t now = millis();
    if (now - last_report < REPORT_MS) return;
    last_report = now;

    portENTER_CRITICAL(&cam_mux);
    uint32_t cf = cam_frames, cdl = cam_dl_ms, cdec = cam_dec_ms, cb = cam_bytes;
    cam_frames = cam_dl_ms = cam_dec_ms = cam_bytes = 0;
    portEXIT_CRITICAL(&cam_mux);

    char stacks[128];
    stack_report(stacks, sizeof(stacks));
    Serial.printf("[perf] refr %u (avg %u ms, max %u ms) loop max %u ms | heap %u min %u big %u | "
                  "cam %u fr, dl %u ms, dec %u ms, %u KB | stack%s\n",
                  (unsigned)refr_count, refr_count ? (unsigned)(refr_total_us / refr_count / 1000) : 0,
                  (unsigned)(refr_max_us / 1000), (unsigned)loop_max_ms, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)cf,
                  cf ? (unsigned)(cdl / cf) : 0, cf ? (unsigned)(cdec / cf) : 0, cf ? (unsigned)(cb / cf / 1024) : 0,
                  stacks);
    refr_count = refr_total_us = refr_max_us = 0;
    loop_max_ms = 0;
}

void perf_camera_frame(uint32_t download_ms, uint32_t decode_ms, uint32_t bytes) {
    portENTER_CRITICAL(&cam_mux);
    cam_frames++;
    cam_dl_ms += download_ms;
    cam_dec_ms += decode_ms;
    cam_bytes += bytes;
    portEXIT_CRITICAL(&cam_mux);
}
