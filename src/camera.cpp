#include "camera.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <JPEGDEC.h>
#include <new>

#include "perf.h"

namespace {

SemaphoreHandle_t lock;
String host;                 // guarded by lock
uint32_t host_gen = 0;       // guarded by lock
volatile bool active = false;
volatile uint32_t frames = 0;
volatile uint32_t shown = 0; // last frame the UI has drawn
volatile uint32_t last_frame_ms = 0; // 0: none from the current host yet
char status[48] = "";        // guarded by lock

uint16_t *fb = nullptr;
volatile int view_w = CAM_W, view_h = CAM_H;
// JPEGDEC: several times faster than tjpgd at 1/8 scale, which is what
// sets the frame rate here. Its state is ~17 KB; allocated on first use.
JPEGDEC *jpeg = nullptr;

struct DecodeCtx {
    WiFiClient *stream;
    int32_t remaining;       // bytes left in body, -1 if unknown
    int crop_x, crop_y;      // top-left of the CAM_W x CAM_H window in scaled-image coords
    bool flip_h, flip_v;
    int vw, vh;              // view size for this frame
    uint32_t wait_us;        // time spent waiting for network data
    uint32_t bytes;
};

void set_status(const char *s) {
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(status, s, sizeof(status));
    xSemaphoreGive(lock);
}

// Reads up to len bytes (buf == nullptr: skip them). Not Stream::readBytes:
// that busy-waits byte by byte without yielding while Wi-Fi data trickles
// in, which starves core 0's idle task and trips the task watchdog.
size_t stream_read(DecodeCtx *ctx, uint8_t *buf, size_t len) {
    if (ctx->remaining >= 0 && (int32_t)len > ctx->remaining) len = ctx->remaining;
    size_t got = 0;
    uint8_t skip[128];
    uint32_t last_data = millis();
    while (got < len) {
        int avail = ctx->stream->available();
        if (avail > 0) {
            size_t want = len - got;
            if ((size_t)avail < want) want = avail;
            if (!buf && want > sizeof(skip)) want = sizeof(skip);
            int n = ctx->stream->read(buf ? buf + got : skip, want);
            if (n > 0) {
                got += n;
                last_data = millis();
                continue;
            }
        }
        if (!ctx->stream->connected() && ctx->stream->available() <= 0) break;
        if (millis() - last_data > 4000) break;
        uint32_t w0 = micros();
        vTaskDelay(1);
        ctx->wait_us += micros() - w0;
    }
    if (ctx->remaining >= 0) ctx->remaining -= got;
    ctx->bytes += got;
    return got;
}

int32_t jpeg_read(JPEGFILE *f, uint8_t *buf, int32_t len) {
    size_t n = stream_read(static_cast<DecodeCtx *>(f->fHandle), buf, len);
    f->iPos += n;
    return n;
}

// The body is a stream, so only forward seeks work (JPEGDEC only seeks
// forward while parsing the headers).
int32_t jpeg_seek(JPEGFILE *f, int32_t pos) {
    if (pos < f->iPos) return -1;
    if (pos > f->iPos) f->iPos += stream_read(static_cast<DecodeCtx *>(f->fHandle), nullptr, pos - f->iPos);
    return f->iPos;
}

// Colour correction for this panel. The decoded frame matches the camera,
// but the TN panel shows mid-tones flat, shadows grey and red too strong.
// An S-curve restores contrast, and red chroma is pulled toward grey.
constexpr float CAM_CONTRAST = 0.6f; // 0 = off, 1 = full smoothstep S-curve
constexpr float CAM_SHADOW_GAMMA = 1.15f; // >1 darkens shadows/mid-tones
constexpr int CAM_RED_CHROMA = 180; // /256 of red's excess over luma kept
uint8_t tone[256];

void build_tone_lut() {
    for (int i = 0; i < 256; i++) {
        float t = powf(i / 255.0f, CAM_SHADOW_GAMMA);
        float s = t * t * (3 - 2 * t);
        float v = t + CAM_CONTRAST * (s - t);
        tone[i] = (uint8_t)constrain((int)(v * 255 + 0.5f), 0, 255);
    }
}

inline uint16_t correct(uint16_t px) {
    int r = (px >> 11) << 3, g = ((px >> 5) & 63) << 2, b = (px & 31) << 3;
    r |= r >> 5;
    g |= g >> 6;
    b |= b >> 5;
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    if (r > y) r = y + (((r - y) * CAM_RED_CHROMA) >> 8);
    r = tone[r];
    g = tone[g];
    b = tone[b];
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

int jpeg_draw(JPEGDRAW *d) {
    DecodeCtx *ctx = static_cast<DecodeCtx *>(d->pUser);
    // Decoding is pure CPU work; without yielding, core 0's idle task
    // starves and the task watchdog reboots the board.
    static uint32_t calls = 0;
    if ((++calls & 31) == 0) vTaskDelay(1);
    for (int y = 0; y < d->iHeight; y++) {
        // (cx, cy) in the CAM_W x CAM_H decoded frame, scaled down
        // (nearest) to the view size.
        int cy = d->y + y - ctx->crop_y;
        if (cy < 0 || cy >= CAM_H) continue;
        int vy = cy * ctx->vh / CAM_H;
        int dy = ctx->flip_v ? ctx->vh - 1 - vy : vy;
        uint16_t *row = fb + dy * ctx->vw;
        const uint16_t *src = d->pPixels + y * d->iWidth;
        for (int x = 0; x < d->iWidthUsed; x++) {
            int cx = d->x + x - ctx->crop_x;
            if (cx < 0 || cx >= CAM_W) continue;
            int vx = cx * ctx->vw / CAM_W;
            row[ctx->flip_h ? ctx->vw - 1 - vx : vx] = correct(src[x]);
        }
    }
    return 1;
}

struct CamConfig {
    String snapshot_url; // absolute
    bool flip_h = false, flip_v = false;
};

String host_only(const String &h) {
    int colon = h.indexOf(':');
    return colon >= 0 ? h.substring(0, colon) : h;
}

// Reads the first enabled webcam from Moonraker's webcam config (the same
// one Mainsail shows), including its flip settings.
bool load_config(const String &h, CamConfig &cfg) {
    String base = h.indexOf(':') >= 0 ? "http://" + h : "http://" + h + ":7125";
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    http.useHTTP10(true);
    if (!http.begin(base + "/server/webcams/list")) return false;
    bool ok = false;
    if (http.GET() == 200) {
        JsonDocument filter;
        JsonObject f = filter["result"]["webcams"][0].to<JsonObject>();
        f["enabled"] = true;
        f["snapshot_url"] = true;
        f["flip_horizontal"] = true;
        f["flip_vertical"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) ==
            DeserializationError::Ok) {
            for (JsonObject cam : doc["result"]["webcams"].as<JsonArray>()) {
                if (!(cam["enabled"] | true)) continue;
                String url = cam["snapshot_url"] | "";
                if (url.isEmpty()) continue;
                // Relative URLs are served by the web frontend (port 80), not Moonraker.
                cfg.snapshot_url = url.startsWith("http") ? url : "http://" + host_only(h) + url;
                cfg.flip_h = cam["flip_horizontal"] | false;
                cfg.flip_v = cam["flip_vertical"] | false;
                ok = true;
                break;
            }
        }
    }
    http.end();
    return ok;
}

enum class Frame { Ok, Stale, Failed };

Frame fetch_frame(const CamConfig &cfg) {
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(4000);
    // HTTP/1.0 so nginx doesn't chunk the body — we decode straight off the
    // raw socket, and chunk-size lines would corrupt the JPEG stream.
    http.useHTTP10(true);
    if (!http.begin(cfg.snapshot_url)) return Frame::Failed;
    uint32_t t_start = millis();
    int code = http.GET();
    if (code != 200) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Camera HTTP %d", code);
        set_status(buf);
        http.end();
        return Frame::Failed;
    }

    WiFiClient *stream = http.getStreamPtr();
    int32_t size = http.getSize();
    DecodeCtx ctx{stream, size, 0, 0, cfg.flip_h, cfg.flip_v, view_w, view_h, 0, 0};

    if (!jpeg) jpeg = new (std::nothrow) JPEGDEC();
    bool ok = false;
    if (!jpeg) {
        set_status("Camera: out of memory");
    } else if (jpeg->open(&ctx, size > 0 ? size : 1 << 24, nullptr, jpeg_read, jpeg_seek, jpeg_draw)) {
        jpeg->setUserPointer(&ctx);
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        // The largest 1/2^n downscale that still covers CAM_W x CAM_H (1/8
        // for 1080p); any excess is center-cropped.
        int w = jpeg->getWidth(), h = jpeg->getHeight();
        int div = 8;
        while (div > 1 && (w / div < CAM_W || h / div < CAM_H)) div /= 2;
        int opts = div == 8 ? JPEG_SCALE_EIGHTH : div == 4 ? JPEG_SCALE_QUARTER : div == 2 ? JPEG_SCALE_HALF : 0;
        ctx.crop_x = max(0, (w / div - CAM_W) / 2);
        ctx.crop_y = max(0, (h / div - CAM_H) / 2);
        uint32_t t0 = millis();
        ok = jpeg->decode(0, 0, opts) == 1;
        // The view changed mid-frame: this one is in the old layout.
        if (ok && (ctx.vw != view_w || ctx.vh != view_h)) {
            jpeg->close();
            http.end();
            return Frame::Stale;
        }
        if (ok) {
            // "download" = time spent waiting for data, the rest is decoding.
            uint32_t total = millis() - t_start, wait = ctx.wait_us / 1000;
            perf_camera_frame(wait, total - wait, ctx.bytes);
        } else {
            Serial.printf("[cam] decode error %d after %ums\n", jpeg->getLastError(), (unsigned)(millis() - t0));
            char buf[48];
            snprintf(buf, sizeof(buf), "Camera decode error %d", jpeg->getLastError());
            set_status(buf);
        }
        jpeg->close();
    } else {
        set_status("Camera: bad JPEG");
        Serial.printf("[cam] JPEG open failed (%d)\n", jpeg->getLastError());
    }
    http.end();
    return ok ? Frame::Ok : Frame::Failed;
}

void cam_task(void *) {
    CamConfig cfg;
    uint32_t seen_gen = UINT32_MAX;
    for (;;) {
        if (!active || WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        String h;
        uint32_t gen;
        xSemaphoreTake(lock, portMAX_DELAY);
        h = host;
        gen = host_gen;
        xSemaphoreGive(lock);

        if (gen != seen_gen || cfg.snapshot_url.isEmpty()) {
            set_status("Looking for camera...");
            if (!load_config(h, cfg)) {
                cfg = CamConfig();
                set_status("No webcam configured");
                // Also runs behind the dashboard: don't keep asking every few
                // seconds, but do react right away to a new printer.
                for (int i = 0; i < 75 && host_gen == gen; i++) vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            seen_gen = gen;
        }

        // Don't decode over a frame that isn't on screen yet (see
        // camera_frame_shown). Short timeout: the UI may not be showing it.
        for (int i = 0; i < 25 && shown != frames; i++) vTaskDelay(pdMS_TO_TICKS(10));
        Frame r = fetch_frame(cfg);
        if (r == Frame::Ok) {
            frames++;
            last_frame_ms = millis() | 1;
            set_status("");
        } else if (r == Frame::Failed) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool started = false;

} // namespace

uint16_t *camera_framebuffer() {
    if (!fb) {
        fb = static_cast<uint16_t *>(heap_caps_malloc(CAM_W * CAM_H * 2, MALLOC_CAP_8BIT));
        if (fb) memset(fb, 0, CAM_W * CAM_H * 2);
    }
    return fb;
}

void camera_set_view(int w, int h) {
    w = constrain(w, 1, CAM_W);
    h = constrain(h, 1, CAM_H);
    if (w == view_w && h == view_h) return;
    view_w = w;
    view_h = h;
    if (fb) memset(fb, 0, CAM_W * CAM_H * 2); // no stale frame in the wrong layout
    frames++;
}

void camera_set_host(const String &h) {
    if (!lock) lock = xSemaphoreCreateMutex();
    xSemaphoreTake(lock, portMAX_DELAY);
    host = h;
    host_gen++;
    last_frame_ms = 0;
    xSemaphoreGive(lock);
}

void camera_set_active(bool a) {
    if (a && !started) {
        if (!lock) lock = xSemaphoreCreateMutex();
        build_tone_lut();
        // Core 1 at idle priority: decoding (~1 s of CPU per frame) then only
        // uses the time the LVGL loop leaves free, and core 0 stays free for
        // Wi-Fi, lwIP and the Moonraker polling.
        xTaskCreatePinnedToCore(cam_task, "camera", 6144, nullptr, 0, nullptr, 1); // ~2.5 KB used
        started = true;
    }
    active = a && camera_framebuffer() != nullptr;
}

uint32_t camera_frame_count() { return frames; }

void camera_frame_shown(uint32_t f) { shown = f; }

bool camera_has_feed() {
    uint32_t t = last_frame_ms;
    return t && millis() - t < 20000;
}

void camera_status(char *buf, size_t n) {
    if (!lock) {
        strlcpy(buf, "", n);
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(buf, status, n);
    xSemaphoreGive(lock);
}
