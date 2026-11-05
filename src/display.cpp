#include "display.h"

#include <lvgl.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <mbedtls/base64.h>

namespace {

TFT_eSPI tft = TFT_eSPI();

// Confirmed by direct hardware probing on the actual unit: touch is on its
// own SPI bus, separate from the display's. (TFT_eSPI's built-in shared-bus
// touch support does not work on this board revision.)
constexpr int TOUCH_CLK_PIN = 25;
constexpr int TOUCH_MOSI_PIN = 32;
constexpr int TOUCH_MISO_PIN = 39;
constexpr int TOUCH_CS_PIN = 33;

// HSPI, deliberately not VSPI: TFT_eSPI defaults to the VSPI hardware
// peripheral for the display. A second SPIClass also bound to VSPI would
// remap that same peripheral's pins out from under the display mid-flight
// (garbage on screen), even though the two use different GPIOs — the
// GPIO-to-peripheral mapping is what collides, not the pin numbers.
SPIClass touch_spi(HSPI);
// No tirq pin: the library only re-polls SPI when its IRQ pin interrupts,
// and stops polling for good if that pin ever misbehaves. Omitting it
// forces unconditional polling on every read instead — confirmed reliable
// by direct testing, whereas the IRQ-gated mode was not.
XPT2046_Touchscreen touchscreen(TOUCH_CS_PIN);

struct TouchCal {
    int16_t x_min, x_max, y_min, y_max;
};
TouchCal cal;

// 20 lines (12.8 KB): more, smaller flushes, but the heap needs the RAM
// more (Wi-Fi, HTTP and the camera all allocate from it at once).
constexpr uint32_t DRAW_BUF_LINES = 20;
uint8_t draw_buf[SCREEN_WIDTH * DRAW_BUF_LINES * 2]; // RGB565

lv_display_t *lv_disp = nullptr;
bool shot_active = false;

void send_shot_area(const lv_area_t *area, const uint8_t *px, size_t len) {
    Serial.printf("\n[shot] %d %d %d %d\n", area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    unsigned char out[520];
    for (size_t off = 0; off < len; off += 384) {
        size_t n = len - off < 384 ? len - off : 384, olen = 0;
        mbedtls_base64_encode(out, sizeof(out) - 1, &olen, px + off, n);
        out[olen] = 0;
        // Offset first: other tasks' log lines can land in the middle.
        Serial.printf("@%u %s\n", (unsigned)off, reinterpret_cast<char *>(out));
    }
}

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    if (shot_active) {
        send_shot_area(area, px_map, w * h * 2);
        if (lv_display_flush_is_last(disp)) {
            Serial.println("[shot] end");
            shot_active = false;
        }
    }

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors(reinterpret_cast<uint16_t *>(px_map), w * h, true);
    tft.endWrite();

    lv_display_flush_ready(disp);
}

} // namespace

void display_request_screenshot() {
    shot_active = true;
    lv_obj_invalidate(lv_screen_active());
}

namespace {

// Pressure on a resistive panel wobbles while a finger rests on it, and a
// single low reading used to count as a release. That broke every
// press-and-hold (the e-stop in particular): the hold timer restarted and
// the hold turned into a short click. So: hysteresis on pressure, and a
// release only counts after several low readings in a row.
constexpr uint16_t Z_PRESS = 400;    // needed to start a touch
constexpr uint16_t Z_HOLD = 200;     // enough to keep one going
constexpr uint8_t RELEASE_READS = 3; // ~100 ms at LVGL's 33 ms input period

void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
    static bool pressed = false;
    static uint8_t low_reads = 0;
    static lv_point_t last = {0, 0};

    TS_Point p = touchscreen.getPoint();
    bool firm = p.z > Z_PRESS;
    if (firm || (pressed && p.z > Z_HOLD)) {
        pressed = true;
        low_reads = 0;
        // Coordinates at light pressure are noisy; only trust firm readings.
        if (firm) {
            last.x = constrain(map(p.x, cal.x_min, cal.x_max, 0, SCREEN_WIDTH - 1), 0, SCREEN_WIDTH - 1);
            last.y = constrain(map(p.y, cal.y_min, cal.y_max, 0, SCREEN_HEIGHT - 1), 0, SCREEN_HEIGHT - 1);
        }
    } else if (pressed && ++low_reads >= RELEASE_READS) {
        pressed = false;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = last;
}

uint32_t tick_cb() {
    return millis();
}

void draw_target(int x, int y, int step) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    char msg[32];
    snprintf(msg, sizeof(msg), "Tap and hold (%d/4)", step);
    tft.drawString(msg, SCREEN_WIDTH / 2, 20, 2);
    tft.drawLine(x - 12, y, x + 12, y, TFT_MAGENTA);
    tft.drawLine(x, y - 12, x, y + 12, TFT_MAGENTA);
    tft.drawCircle(x, y, 6, TFT_MAGENTA);
}

// True only after N consecutive reads agree (rejects transient SPI/power-on
// noise, which a single high reading right after touchscreen.begin() can
// produce).
bool stable_read(bool want_pressed, TS_Point &out, int samples = 5) {
    for (int i = 0; i < samples; i++) {
        TS_Point p = touchscreen.getPoint();
        bool pressed = p.z > 500;
        if (pressed != want_pressed) return false;
        out = p;
        delay(20);
    }
    return true;
}

// Blocks until the panel is firmly and steadily pressed, then averages many
// samples over a further ~250ms hold to cancel out resistive-panel jitter
// (a single reading is noisy enough to make calibration noticeably
// inaccurate). Blocks again until released before returning.
TS_Point wait_for_tap() {
    TS_Point p;
    while (!stable_read(false, p)) delay(20); // wait for a stable "not pressed" baseline
    while (!stable_read(true, p)) delay(20);  // wait for a stable press to begin

    long sx = 0, sy = 0;
    constexpr int SAMPLES = 25;
    for (int i = 0; i < SAMPLES; i++) {
        TS_Point s = touchscreen.getPoint();
        sx += s.x;
        sy += s.y;
        delay(10);
    }
    TS_Point avg(sx / SAMPLES, sy / SAMPLES, 0);

    while (!stable_read(false, p)) delay(20); // wait for release before returning
    return avg;
}

// 4-corner calibration (inset from the true edges so the crosshair is fully
// on-screen), each point heavily averaged (see wait_for_tap). Using all 4
// corners — rather than just 2 diagonal ones — cancels out per-edge
// non-linearity/tilt in the resistive panel instead of trusting a single
// diagonal to represent the whole surface. Robust to an inverted axis
// (min/max taken regardless of which side reads higher), but assumes no
// axis swap — touchscreen.setRotation(1) is chosen specifically because
// it's the one rotation value that doesn't swap x/y.
void run_calibration() {
    constexpr int INSET = 24;

    draw_target(INSET, INSET, 1);
    TS_Point tl = wait_for_tap();

    draw_target(SCREEN_WIDTH - INSET, INSET, 2);
    TS_Point tr = wait_for_tap();

    draw_target(INSET, SCREEN_HEIGHT - INSET, 3);
    TS_Point bl = wait_for_tap();

    draw_target(SCREEN_WIDTH - INSET, SCREEN_HEIGHT - INSET, 4);
    TS_Point br = wait_for_tap();

    int16_t x_lo = min(min(tl.x, bl.x), min(tr.x, br.x));
    int16_t x_hi = max(max(tl.x, bl.x), max(tr.x, br.x));
    int16_t y_lo = min(min(tl.y, tr.y), min(bl.y, br.y));
    int16_t y_hi = max(max(tl.y, tr.y), max(bl.y, br.y));

    // The targets are inset from the edges, so extrapolate the raw range
    // back out to the true 0..SCREEN_WIDTH/HEIGHT edges proportionally.
    int32_t x_span = x_hi - x_lo;
    int32_t y_span = y_hi - y_lo;
    int32_t x_pad = x_span * INSET / (SCREEN_WIDTH - 2 * INSET);
    int32_t y_pad = y_span * INSET / (SCREEN_HEIGHT - 2 * INSET);

    cal.x_min = x_lo - x_pad;
    cal.x_max = x_hi + x_pad;
    cal.y_min = y_lo - y_pad;
    cal.y_max = y_hi + y_pad;
}

void load_or_run_calibration() {
    Preferences prefs;
    prefs.begin("touch", false);
    size_t got = prefs.getBytes("cal5", &cal, sizeof(cal));
    if (got != sizeof(cal)) {
        run_calibration();
        prefs.putBytes("cal5", &cal, sizeof(cal));
    }
    prefs.end();
}

} // namespace

void display_init() {
    tft.init();
    tft.setRotation(1); // landscape
    {
        // An earlier build stored a panel voltage/gamma preset here; those
        // register writes garbled this panel. Drop the stale key.
        Preferences p;
        p.begin("touch", false);
        p.remove("panel");
        p.end();
    }
    tft.fillScreen(TFT_BLACK);

    touch_spi.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
    touchscreen.begin(touch_spi);
    touchscreen.setRotation(1);
    delay(200); // let the bus settle before trusting any readings

    load_or_run_calibration();

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(lv_disp, flush_cb);
    lv_display_set_buffers(lv_disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Dark theme globally, so built-in widgets (keyboard, list, spinner)
    // match the custom-styled dark screens instead of LVGL's default light
    // look.
    lv_theme_t *theme = lv_theme_default_init(
        lv_disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL),
        /*dark=*/true, &lv_font_montserrat_14);
    lv_display_set_theme(lv_disp, theme);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
}
