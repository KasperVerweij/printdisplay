#include "ui.h"

#include <lvgl.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <Preferences.h>
#include <WiFi.h>

#include "camera.h"
#include "display.h"
#include "lan_finder.h"
#include "moonraker.h"
#include "printer_scan.h"
#include "printer_api.h"
#include "ui_common.h"

namespace {

// First-boot default only; pick the printer under ⚙ → Choose printer.
constexpr const char *DEFAULT_PRINTER_HOST = "192.168.0.67";
// The printer Pi's Wi-Fi MAC (first-boot default; learned from whatever
// answers at a host you enter). DHCP on this network moves the printer, so
// the display finds it again by MAC when its IP stops answering.
constexpr const char *DEFAULT_PRINTER_MAC = "b8:27:eb:20:83:84";


Preferences prefs;
String printer_host;

lv_obj_t *scr_list;
lv_obj_t *scr_entry;
lv_obj_t *scr_connecting;
lv_obj_t *scr_dashboard;
lv_obj_t *scr_settings;
lv_obj_t *scr_camera;

lv_obj_t *list_widget;
lv_obj_t *entry_title;
lv_obj_t *entry_ta;
lv_obj_t *connecting_label;

// dashboard widgets
lv_obj_t *state_dot;
lv_obj_t *state_label;
lv_obj_t *file_label;
lv_obj_t *progress_arc;
lv_obj_t *pct_label;
lv_obj_t *layer_label;
struct HeaterWidgets {
    lv_obj_t *card;
    lv_obj_t *value;
    lv_obj_t *target;
    lv_obj_t *bar;
};
HeaterWidgets nozzle_w, bed_w;
lv_obj_t *elapsed_label;
lv_obj_t *remaining_label;

// The dashboard has two layouts: without a camera feed (big progress arc,
// heaters + times on the right) and with one (camera top left, small arc
// next to it, heaters along the bottom, times over the camera image).
lv_obj_t *arc_card;
lv_obj_t *time_card;
lv_obj_t *dash_cam_img;
lv_obj_t *dash_cam_times;
int dash_layout = -1; // 0 = no camera, 1 = camera

lv_obj_t *settings_info;

// printer list screen
lv_obj_t *scr_printers;
lv_obj_t *printer_list;
lv_obj_t *printers_status;
lv_timer_t *printers_timer = nullptr;
FoundPrinter found_printers[PRINTER_SCAN_MAX];
uint32_t shown_printers = 0; // signature of what the list shows; 0 = rebuild

// Play/pause/restart + e-stop exist on both the dashboard and the camera
// screen; index 0 = dashboard, 1 = camera.
enum class Action { None, Pause, Resume, FirmwareRestart };
Action current_action = Action::None;
lv_obj_t *action_btns[2];
lv_obj_t *action_lbls[2];

// camera screen
lv_obj_t *cam_img;
lv_obj_t *cam_info;
lv_obj_t *cam_status;
lv_image_dsc_t cam_dsc;  // camera screen: the frame buffer at CAM_W x CAM_H
lv_image_dsc_t dash_dsc; // dashboard tile: the same buffer at DASH_CAM_W x DASH_CAM_H
constexpr int DASH_CAM_W = 192, DASH_CAM_H = 108;

void init_dsc(lv_image_dsc_t &d, const uint16_t *fb, int w, int h) {
    memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = w;
    d.header.h = h;
    d.header.stride = w * 2;
    d.data_size = w * h * 2;
    d.data = reinterpret_cast<const uint8_t *>(fb);
}
uint32_t cam_seen_frames = 0;
constexpr uint32_t RESULT_SHOW_MS = 4000;

lv_timer_t *connect_poll_timer = nullptr;
lv_timer_t *dashboard_timer = nullptr;

constexpr int MAX_NETWORKS = 20;
String scanned_ssids[MAX_NETWORKS];

enum class EntryMode { WifiPassword, PrinterHost };
EntryMode entry_mode = EntryMode::WifiPassword;

String selected_ssid;
String pending_password;
bool moonraker_started = false;

uint32_t connect_start_ms = 0;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

lv_obj_t *temp_screen = nullptr; // see ui_load_temp()

void load_screen(lv_obj_t *scr) {
    lv_obj_t *prev = lv_screen_active();
    lv_screen_load(scr);
    if (prev && prev == temp_screen && prev != scr) {
        lv_obj_delete_async(prev);
        temp_screen = nullptr;
    }
}

void show_wifi_list();
void show_password_screen(const String &ssid);
void show_host_screen();
void show_connecting(const String &ssid, const String &pass);
void show_dashboard();
void show_settings();
void show_camera();
void show_printers();

// ---------------------------------------------------------------- helpers

void format_duration(char *buf, size_t n, float secs) {
    if (secs < 0) secs = 0;
    uint32_t s = (uint32_t)secs;
    uint32_t h = s / 3600, m = (s / 60) % 60;
    if (h > 0)
        snprintf(buf, n, "%uh %02um", (unsigned)h, (unsigned)m);
    else
        snprintf(buf, n, "%um %02us", (unsigned)m, (unsigned)(s % 60));
}

const char *strip_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---------------------------------------------------------------- wifi

void populate_list(int n) {
    lv_obj_clean(list_widget);
    int count = (n > 0) ? min(n, MAX_NETWORKS) : 0;

    if (count == 0) {
        lv_list_add_text(list_widget, "No networks found");
    } else {
        for (int i = 0; i < count; i++) {
            scanned_ssids[i] = WiFi.SSID(i);
            String label = scanned_ssids[i].length() ? scanned_ssids[i] : String("(hidden)");
            char line[48];
            snprintf(line, sizeof(line), "%s  %ddBm", label.c_str(), WiFi.RSSI(i));

            lv_obj_t *btn = lv_list_add_button(list_widget, LV_SYMBOL_WIFI, line);
            lv_obj_add_event_cb(
                btn,
                [](lv_event_t *e) {
                    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                    show_password_screen(scanned_ssids[idx]);
                },
                LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        }
    }
    WiFi.scanDelete();
}

void poll_connect(lv_timer_t *) {
    if (WiFi.status() == WL_CONNECTED) {
        lv_timer_delete(connect_poll_timer);
        connect_poll_timer = nullptr;
        prefs.putString("ssid", selected_ssid);
        prefs.putString("pass", pending_password);
        show_dashboard();
        return;
    }
    if (millis() - connect_start_ms > CONNECT_TIMEOUT_MS) {
        lv_timer_delete(connect_poll_timer);
        connect_poll_timer = nullptr;
        WiFi.disconnect();
        show_password_screen(selected_ssid);
        lv_label_set_text_fmt(entry_title, "Failed, retry: %s", selected_ssid.c_str());
    }
}

// ---------------------------------------------------------------- dashboard

// The dashboard refreshes every 500 ms, mostly with the same values. In
// LVGL, setting a label's text (even to the same string) or a style
// redraws the widget and whatever is under it, and restarts a scrolling
// label's animation. So only touch what actually changed.
void set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void set_textf(lv_obj_t *label, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_text(label, buf);
}

void set_text_color(lv_obj_t *obj, lv_color_t color) {
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) lv_obj_set_style_text_color(obj, color, 0);
}

void show_if(lv_obj_t *obj, bool visible) {
    if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (visible)
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_state(const char *text, lv_color_t color) {
    set_text(state_label, text);
    set_text_color(state_label, color);
    if (!lv_color_eq(lv_obj_get_style_bg_color(state_dot, LV_PART_MAIN), color)) lv_obj_set_style_bg_color(state_dot, color, 0);
}

void set_heater(HeaterWidgets &w, float temp, float target, float power) {
    set_textf(w.value, "%.1f\xC2\xB0", temp);
    if (target > 0)
        set_textf(w.target, "/ %.0f\xC2\xB0", target);
    else
        set_text(w.target, "off");
    lv_bar_set_value(w.bar, (int32_t)(power * 100 + 0.5f), LV_ANIM_OFF);
}

void style_button(lv_obj_t *btn, lv_color_t color) {
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c3a4a), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, lv_color_white(), 0);
    lv_obj_set_style_text_color(btn, MUTED, LV_STATE_DISABLED);
}

void update_action_buttons(const PrinterStatus &s) {
    if (!s.reachable)
        current_action = Action::None;
    else if (strcmp(s.klippy_state, "shutdown") == 0 || strcmp(s.klippy_state, "error") == 0)
        current_action = Action::FirmwareRestart;
    else if (!strcmp(s.state, "printing"))
        current_action = Action::Pause;
    else if (!strcmp(s.state, "paused"))
        current_action = Action::Resume;
    else
        current_action = Action::None;

    const char *sym = current_action == Action::Resume            ? LV_SYMBOL_PLAY
                      : current_action == Action::FirmwareRestart ? LV_SYMBOL_REFRESH
                                                                   : LV_SYMBOL_PAUSE;
    lv_color_t col = current_action == Action::FirmwareRestart ? WARN
                     : current_action == Action::Resume         ? GOOD
                                                                : ACCENT;
    for (int i = 0; i < 2; i++) {
        set_text(action_lbls[i], sym);
        // Not read back from the style: while disabled that returns the disabled color.
        static bool styled[2] = {false, false};
        static lv_color_t styled_col[2];
        if (!styled[i] || !lv_color_eq(styled_col[i], col)) {
            style_button(action_btns[i], col);
            styled[i] = true;
            styled_col[i] = col;
        }
        if (current_action == Action::None)
            lv_obj_add_state(action_btns[i], LV_STATE_DISABLED);
        else
            lv_obj_remove_state(action_btns[i], LV_STATE_DISABLED);
    }
}

// Short-lived UI messages ("Hold to ..."). They used to be written straight
// into the labels, and the next 500 ms refresh overwrote them before anyone
// could read them; now the refresh shows the newest of this and the last
// command result for RESULT_SHOW_MS.
char ui_notice[48] = "";
uint32_t ui_notice_ms = 0;

bool current_notice(char *buf, size_t n) {
    char res[48];
    uint32_t res_t = moonraker_last_result(res, sizeof(res));
    uint32_t now = millis();
    bool res_ok = res_t && now - res_t < RESULT_SHOW_MS;
    bool ui_ok = ui_notice_ms && now - ui_notice_ms < RESULT_SHOW_MS;
    if (res_ok && (!ui_ok || (int32_t)(res_t - ui_notice_ms) >= 0))
        strlcpy(buf, res, n);
    else if (ui_ok)
        strlcpy(buf, ui_notice, n);
    else
        return false;
    return true;
}

void update_dashboard(lv_timer_t *);

void flash_message(const char *text) {
    strlcpy(ui_notice, text, sizeof(ui_notice));
    ui_notice_ms = millis();
    update_dashboard(nullptr);
}

void on_action_click(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    switch (current_action) {
    case Action::Pause:
        if (code == LV_EVENT_SHORT_CLICKED) moonraker_send(PrinterCommand::Pause);
        break;
    case Action::Resume:
        if (code == LV_EVENT_SHORT_CLICKED) moonraker_send(PrinterCommand::Resume);
        break;
    case Action::FirmwareRestart:
        // Restart is disruptive enough to want a deliberate hold.
        if (code == LV_EVENT_LONG_PRESSED) moonraker_send(PrinterCommand::FirmwareRestart);
        else if (code == LV_EVENT_SHORT_CLICKED) flash_message("Hold to firmware-restart");
        break;
    case Action::None:
        break;
    }
}

void on_estop(lv_event_t *e) {
    // Press-and-hold (~0.4 s) so a stray tap on a resistive panel doesn't
    // kill a print. Feedback starts at touch-down so it's obvious a hold
    // is needed.
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: flash_message("Keep holding for E-STOP..."); break;
    case LV_EVENT_LONG_PRESSED: moonraker_send(PrinterCommand::EmergencyStop); update_dashboard(nullptr); break;
    case LV_EVENT_SHORT_CLICKED: flash_message("Hold the red button to E-STOP"); break;
    default: break;
    }
}

lv_obj_t *make_icon_button(lv_obj_t *parent, const char *sym, int w, int h, lv_obj_t **out_lbl = nullptr) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_t *lbl = lv_label_create(btn);
    set_text(lbl, sym);
    lv_obj_center(lbl);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

lv_obj_t *make_action_button(lv_obj_t *parent, int idx, int w, int h) {
    lv_obj_t *btn = make_icon_button(parent, LV_SYMBOL_PAUSE, w, h, &action_lbls[idx]);
    lv_obj_add_event_cb(btn, on_action_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, on_action_click, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    action_btns[idx] = btn;
    return btn;
}

lv_obj_t *make_estop_button(lv_obj_t *parent, int w, int h) {
    lv_obj_t *btn = make_icon_button(parent, LV_SYMBOL_STOP, w, h);
    style_button(btn, BAD);
    // Unmistakable while held.
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xff6040), LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(btn, 3, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_estop, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, on_estop, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, on_estop, LV_EVENT_LONG_PRESSED, NULL);
    return btn;
}

void update_camera_screen(const PrinterStatus &s) {
    if (lv_screen_active() != scr_camera) return;

    if (s.reachable)
        set_textf(cam_info, "%s  %d%%   %.0f\xC2\xB0 / %.0f\xC2\xB0", lv_label_get_text(state_label),
                              (int)(s.progress * 100), s.extruder_temp, s.bed_temp);
    else
        set_text(cam_info, lv_label_get_text(state_label));

    char res[48];
    char st[48];
    camera_status(st, sizeof(st));
    if (current_notice(res, sizeof(res)))
        set_text(cam_status, res);
    else if (st[0])
        set_text(cam_status, st);
    else if (cam_seen_frames == 0)
        set_text(cam_status, "Loading camera...");
    else
        set_text(cam_status, "");
    show_if(cam_status, lv_label_get_text(cam_status)[0]);
}

void set_dash_layout(bool cam);

void update_dash_camera() {
    set_dash_layout(camera_has_feed());
}

// There's one frame buffer and no RAM for a second one, so the camera task
// waits after each frame until it's on screen (camera_frame_shown) before
// decoding the next into the same buffer. Otherwise the screen shows bands
// of two frames. Checked often, so the camera waits as little as possible.
void cam_tick(lv_timer_t *) {
    uint32_t f = camera_frame_count();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *img = scr == scr_camera ? cam_img : (scr == scr_dashboard && dash_layout == 1) ? dash_cam_img : nullptr;
    if (img && f != cam_seen_frames) {
        lv_image_cache_drop(scr == scr_camera ? &cam_dsc : &dash_dsc);
        lv_obj_invalidate(img);
        lv_refr_now(nullptr); // draw it now, then let the camera go on
    }
    cam_seen_frames = f;
    camera_frame_shown(f);
}

void set_dash_layout(bool cam) {
    if (dash_layout == (int)cam) return;
    dash_layout = cam;
    if (cam) {
        if (!camera_framebuffer()) return;
        lv_image_set_src(dash_cam_img, &dash_dsc);
        lv_obj_remove_flag(dash_cam_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(time_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(arc_card, 206, 64);
        lv_obj_set_size(arc_card, 106, 108);
        lv_obj_set_size(progress_arc, 82, 82);
        lv_obj_align(progress_arc, LV_ALIGN_TOP_MID, 0, -2);
        lv_obj_set_style_arc_width(progress_arc, 7, LV_PART_MAIN);
        lv_obj_set_style_arc_width(progress_arc, 7, LV_PART_INDICATOR);
        lv_obj_set_style_text_font(pct_label, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(nozzle_w.card, 8, 178);
        lv_obj_set_size(nozzle_w.card, 148, 56);
        lv_obj_set_pos(bed_w.card, 164, 178);
        lv_obj_set_size(bed_w.card, 148, 56);
    } else {
        lv_obj_add_flag(dash_cam_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(time_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(arc_card, 8, 68);
        lv_obj_set_size(arc_card, 136, 164);
        lv_obj_set_size(progress_arc, 120, 120);
        lv_obj_align(progress_arc, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_set_style_arc_width(progress_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_width(progress_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_text_font(pct_label, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(nozzle_w.card, 150, 68);
        lv_obj_set_size(nozzle_w.card, 162, 54);
        lv_obj_set_pos(bed_w.card, 150, 126);
        lv_obj_set_size(bed_w.card, 162, 54);
    }
}

void update_dashboard(lv_timer_t *) {
    PrinterStatus s;
    moonraker_get(s);

    // The polling task moves to the printer's new IP on its own; keep it.
    String mh = moonraker_host();
    if (mh.length() && mh != printer_host) {
        printer_host = mh;
        prefs.putString("host", printer_host);
        camera_set_host(printer_host);
    }

    if (WiFi.status() != WL_CONNECTED) {
        set_state("Wi-Fi lost", BAD);
    } else if (!s.reachable) {
        set_state("Printer offline", BAD);
    } else if (strcmp(s.klippy_state, "ready") != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Klipper %s", s.klippy_state);
        set_state(buf, strcmp(s.klippy_state, "startup") == 0 ? WARN : BAD);
    } else if (!strcmp(s.state, "printing")) {
        set_state("Printing", ACCENT);
    } else if (!strcmp(s.state, "paused")) {
        set_state("Paused", WARN);
    } else if (!strcmp(s.state, "complete")) {
        set_state("Complete", GOOD);
    } else if (!strcmp(s.state, "cancelled")) {
        set_state("Cancelled", BAD);
    } else if (!strcmp(s.state, "error")) {
        set_state("Error", BAD);
    } else {
        set_state("Standby", MUTED);
    }

    update_action_buttons(s);
    update_camera_screen(s);
    update_dash_camera();

    char res[48];
    bool show_result = current_notice(res, sizeof(res));

    if (!s.reachable) {
        if (show_result)
            set_text(file_label, res);
        else if (s.searching)
            set_text(file_label, "Looking for the printer on the network...");
        else if (s.last_http_code == 401 || s.last_http_code == 403)
            // Moonraker doesn't trust this display's address.
            set_textf(file_label, "%s: HTTP %d, add me to trusted_clients", printer_host.c_str(),
                                  s.last_http_code);
        else if (s.last_http_code > 0)
            set_textf(file_label, "%s: HTTP %d", printer_host.c_str(), s.last_http_code);
        else
            set_textf(file_label, "No response from %s", printer_host.c_str());
        return; // keep last known numbers on screen
    }

    bool active = !strcmp(s.state, "printing") || !strcmp(s.state, "paused");
    bool has_job = active || !strcmp(s.state, "complete") || !strcmp(s.state, "cancelled") ||
                   !strcmp(s.state, "error");

    if (show_result)
        set_text(file_label, res);
    else if (s.message[0] && !active)
        set_text(file_label, s.message);
    else if (has_job && s.filename[0])
        set_text(file_label, strip_dir(s.filename));
    else
        set_text(file_label, "No active print");

    float progress = !strcmp(s.state, "complete") ? 1.0f : (has_job ? s.progress : 0.0f);
    int pct = (int)(progress * 100.0f);
    lv_arc_set_value(progress_arc, (int32_t)(progress * 1000));
    set_textf(pct_label, "%d%%", pct);
    lv_color_t arc_color = !strcmp(s.state, "complete") ? GOOD
                           : !strcmp(s.state, "paused") ? WARN
                           : (!strcmp(s.state, "error") || !strcmp(s.state, "cancelled")) ? BAD
                                                                                          : ACCENT;
    if (!lv_color_eq(lv_obj_get_style_arc_color(progress_arc, LV_PART_INDICATOR), arc_color))
        lv_obj_set_style_arc_color(progress_arc, arc_color, LV_PART_INDICATOR);

    if (has_job && s.current_layer > 0 && s.total_layer > 0)
        set_textf(layer_label, "layer %d/%d", s.current_layer, s.total_layer);
    else if (has_job && s.current_layer > 0)
        set_textf(layer_label, "layer %d", s.current_layer);
    else
        set_text(layer_label, "");

    set_heater(nozzle_w, s.extruder_temp, s.extruder_target, s.extruder_power);
    set_heater(bed_w, s.bed_temp, s.bed_target, s.bed_power);

    char buf[24];
    if (has_job) {
        format_duration(buf, sizeof(buf), s.total_duration);
        set_text(elapsed_label, buf);
    } else {
        set_text(elapsed_label, "--");
    }

    // Remaining: average the slicer estimate and the file-progress
    // extrapolation once both are meaningful (similar to Mainsail).
    float remaining = -1;
    if (active) {
        float by_file = -1, by_slicer = -1;
        if (s.progress > 0.02f && s.print_duration > 0)
            by_file = s.print_duration / s.progress - s.print_duration;
        if (s.slicer_estimate_s > 0) by_slicer = max(0.0f, s.slicer_estimate_s - s.print_duration);
        if (by_file >= 0 && by_slicer >= 0)
            remaining = (by_file + by_slicer) / 2;
        else
            remaining = by_file >= 0 ? by_file : by_slicer;
    } else if (!strcmp(s.state, "complete")) {
        remaining = 0;
    }
    if (remaining >= 0) {
        format_duration(buf, sizeof(buf), remaining);
        set_text(remaining_label, buf);
    } else {
        set_text(remaining_label, "--");
    }
    if (has_job)
        set_textf(dash_cam_times, "%s  /  %s left", lv_label_get_text(elapsed_label),
                              lv_label_get_text(remaining_label));
    else
        set_text(dash_cam_times, "");
    show_if(dash_cam_times, lv_label_get_text(dash_cam_times)[0]);
}

// ---------------------------------------------------------------- screens

void build_wifi_list_screen() {
    scr_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_list, BG, 0);

    lv_obj_t *title = lv_label_create(scr_list);
    lv_label_set_text(title, "Select Wi-Fi network");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

    lv_obj_t *rescan_btn = lv_button_create(scr_list);
    lv_obj_align(rescan_btn, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_lbl, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_add_event_cb(rescan_btn, [](lv_event_t *) { show_wifi_list(); }, LV_EVENT_CLICKED, NULL);

    list_widget = lv_list_create(scr_list);
    lv_obj_set_size(list_widget, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 50);
    lv_obj_align(list_widget, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// Shared keyboard screen for both the Wi-Fi password and the printer address.
void build_entry_screen() {
    scr_entry = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_entry, BG, 0);

    entry_title = lv_label_create(scr_entry);
    lv_obj_set_style_text_color(entry_title, lv_color_white(), 0);
    lv_obj_align(entry_title, LV_ALIGN_TOP_MID, 0, 4);

    entry_ta = lv_textarea_create(scr_entry);
    lv_textarea_set_one_line(entry_ta, true);
    lv_obj_set_width(entry_ta, SCREEN_WIDTH - 20);
    lv_obj_align(entry_ta, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *kb = lv_keyboard_create(scr_entry);
    lv_keyboard_set_textarea(kb, entry_ta);
    lv_obj_set_size(kb, SCREEN_WIDTH, SCREEN_HEIGHT - 70);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(
        kb,
        [](lv_event_t *e) {
            lv_event_code_t code = lv_event_get_code(e);
            String text = lv_textarea_get_text(entry_ta);
            if (entry_mode == EntryMode::WifiPassword) {
                if (code == LV_EVENT_READY) show_connecting(selected_ssid, text);
                else if (code == LV_EVENT_CANCEL) show_wifi_list();
            } else if (entry_mode == EntryMode::PrinterHost) {
                if (code == LV_EVENT_READY) {
                    text.trim();
                    uint8_t mac[6];
                    if (lan_parse_mac(text, mac)) {
                        moonraker_set_printer_mac(mac); // find it by MAC instead
                    } else if (text.length()) {
                        printer_host = text;
                        prefs.putString("host", printer_host);
                        moonraker_set_host(printer_host);
                        camera_set_host(printer_host);
                    }
                    show_dashboard();
                } else if (code == LV_EVENT_CANCEL) {
                    show_settings();
                }
            }
        },
        LV_EVENT_ALL, NULL);
}

void build_connecting_screen() {
    scr_connecting = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_connecting, BG, 0);
    lv_obj_set_flex_flow(scr_connecting, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr_connecting, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *spinner = lv_spinner_create(scr_connecting);
    lv_obj_set_size(spinner, 50, 50);

    connecting_label = lv_label_create(scr_connecting);
    lv_obj_set_style_text_color(connecting_label, lv_color_white(), 0);
    lv_obj_set_style_pad_top(connecting_label, 16, 0);
}

lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, CARD_BG, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

HeaterWidgets build_heater_card(const char *caption, int y) {
    lv_obj_t *card = make_card(scr_dashboard, 150, y, 162, 54);
    HeaterWidgets w;
    w.card = card;

    lv_obj_t *cap = make_label(card, caption, &lv_font_montserrat_14, MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 2, 0);

    w.target = make_label(card, "--", &lv_font_montserrat_14, MUTED);
    lv_obj_align(w.target, LV_ALIGN_TOP_RIGHT, -2, 0);

    w.value = make_label(card, "--", &lv_font_montserrat_24, lv_color_white());
    lv_obj_align(w.value, LV_ALIGN_BOTTOM_LEFT, 2, -4);

    // Heater power (PWM duty), a quick "is it working hard" hint.
    w.bar = lv_bar_create(card);
    lv_obj_set_size(w.bar, 40, 6);
    lv_obj_align(w.bar, LV_ALIGN_BOTTOM_RIGHT, -2, -10);
    lv_bar_set_range(w.bar, 0, 100);
    lv_obj_set_style_bg_color(w.bar, lv_color_hex(0x2c3a4a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w.bar, HOT, LV_PART_INDICATOR);
    return w;
}

void build_dashboard_screen() {
    scr_dashboard = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_dashboard, BG, 0);
    lv_obj_remove_flag(scr_dashboard, LV_OBJ_FLAG_SCROLLABLE);

    // --- top bar: state + settings
    state_dot = lv_obj_create(scr_dashboard);
    lv_obj_set_size(state_dot, 12, 12);
    lv_obj_set_style_radius(state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(state_dot, 0, 0);
    lv_obj_set_pos(state_dot, 12, 15);

    state_label = make_label(scr_dashboard, "Connecting...", &lv_font_montserrat_18, MUTED);
    lv_label_set_long_mode(state_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(state_label, 130);
    lv_obj_set_pos(state_label, 30, 10);

    // Right to left: menu (everything else), e-stop, play/pause/restart, camera.
    lv_obj_t *settings_btn = make_icon_button(scr_dashboard, LV_SYMBOL_LIST, 36, 36);
    lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_add_event_cb(settings_btn, [](lv_event_t *) { menu_show(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *estop = make_estop_button(scr_dashboard, 36, 36);
    lv_obj_align_to(estop, settings_btn, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    lv_obj_t *action = make_action_button(scr_dashboard, 0, 36, 36);
    lv_obj_align_to(action, estop, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    lv_obj_t *cam_btn = make_icon_button(scr_dashboard, LV_SYMBOL_IMAGE, 36, 36);
    lv_obj_align_to(cam_btn, action, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_add_event_cb(cam_btn, [](lv_event_t *) { show_camera(); }, LV_EVENT_CLICKED, NULL);

    // --- filename (scrolls if too long)
    file_label = make_label(scr_dashboard, "", &lv_font_montserrat_14, lv_color_white());
    lv_label_set_long_mode(file_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(file_label, SCREEN_WIDTH - 24);
    lv_obj_set_pos(file_label, 12, 44);

    // --- progress arc
    arc_card = make_card(scr_dashboard, 8, 68, 136, 164);
    progress_arc = lv_arc_create(arc_card);
    lv_obj_set_size(progress_arc, 120, 120);
    lv_obj_align(progress_arc, LV_ALIGN_TOP_MID, 0, 2);
    lv_arc_set_range(progress_arc, 0, 1000);
    lv_arc_set_bg_angles(progress_arc, 135, 45);
    lv_arc_set_value(progress_arc, 0);
    lv_obj_remove_style(progress_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(progress_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(progress_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(progress_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(progress_arc, lv_color_hex(0x2c3a4a), LV_PART_MAIN);
    lv_obj_set_style_arc_color(progress_arc, ACCENT, LV_PART_INDICATOR);

    pct_label = make_label(progress_arc, "0%", &lv_font_montserrat_24, lv_color_white());
    lv_obj_center(pct_label);

    layer_label = make_label(arc_card, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(layer_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    // --- heaters
    nozzle_w = build_heater_card("NOZZLE", 68);
    bed_w = build_heater_card("BED", 126);

    // --- times
    time_card = make_card(scr_dashboard, 150, 184, 162, 48);
    lv_obj_t *el_cap = make_label(time_card, "ELAPSED", &lv_font_montserrat_14, MUTED);
    lv_obj_align(el_cap, LV_ALIGN_TOP_LEFT, 2, -2);
    elapsed_label = make_label(time_card, "--", &lv_font_montserrat_14, lv_color_white());
    lv_obj_align(elapsed_label, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    lv_obj_t *rem_cap = make_label(time_card, "LEFT", &lv_font_montserrat_14, MUTED);
    lv_obj_align(rem_cap, LV_ALIGN_TOP_RIGHT, -2, -2);
    remaining_label = make_label(time_card, "--", &lv_font_montserrat_14, lv_color_white());
    lv_obj_align(remaining_label, LV_ALIGN_BOTTOM_RIGHT, -2, 0);

    // --- camera (only shown while there's a feed); shares the camera screen's frame
    dash_cam_img = lv_image_create(scr_dashboard);
    lv_obj_set_pos(dash_cam_img, 8, 64);
    // Rendered at this size by the camera task: no LVGL scaling and no
    // rounded clip, which both need large scratch buffers on every redraw.
    lv_obj_set_size(dash_cam_img, DASH_CAM_W, DASH_CAM_H);
    lv_obj_add_flag(dash_cam_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dash_cam_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(dash_cam_img, [](lv_event_t *) { show_camera(); }, LV_EVENT_CLICKED, NULL);

    dash_cam_times = make_label(dash_cam_img, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_style_bg_color(dash_cam_times, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dash_cam_times, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(dash_cam_times, 4, 0);
    lv_obj_set_style_radius(dash_cam_times, 4, 0);
    lv_obj_align(dash_cam_times, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    set_state("Connecting...", MUTED);
}

void build_camera_screen() {
    scr_camera = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_camera, lv_color_black(), 0);
    lv_obj_remove_flag(scr_camera, LV_OBJ_FLAG_SCROLLABLE);

    uint16_t *fb = camera_framebuffer();
    init_dsc(cam_dsc, fb, CAM_W, CAM_H);
    init_dsc(dash_dsc, fb, DASH_CAM_W, DASH_CAM_H);

    // Shown 1:1 (no LVGL scaling), centered above the buttons.
    cam_img = lv_image_create(scr_camera);
    lv_obj_set_size(cam_img, CAM_W, CAM_H);
    lv_obj_align(cam_img, LV_ALIGN_TOP_MID, 0, 26);
    if (fb) lv_image_set_src(cam_img, &cam_dsc);

    cam_info = make_label(scr_camera, "", &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_style_bg_color(cam_info, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cam_info, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(cam_info, 4, 0);
    lv_obj_set_pos(cam_info, 4, 4);

    cam_status = make_label(scr_camera, fb ? "Loading camera..." : "Out of memory", &lv_font_montserrat_14,
                            lv_color_white());
    lv_obj_set_style_bg_color(cam_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cam_status, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(cam_status, 6, 0);
    lv_obj_align(cam_status, LV_ALIGN_TOP_MID, 0, 75);

    // Bottom bar: back, play/pause/restart, e-stop.
    constexpr int BW = 96, BH = 48, Y = 186, GAP = 8;
    lv_obj_t *back = make_icon_button(scr_camera, LV_SYMBOL_LEFT, BW, BH);
    lv_obj_set_pos(back, GAP, Y);
    lv_obj_add_event_cb(back, [](lv_event_t *) { show_dashboard(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *action = make_action_button(scr_camera, 1, BW, BH);
    lv_obj_set_pos(action, GAP + (BW + GAP), Y);

    lv_obj_t *estop = make_estop_button(scr_camera, BW, BH);
    lv_obj_set_pos(estop, GAP + 2 * (BW + GAP), Y);
}

void build_settings_screen() {
    scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_settings, BG, 0);
    lv_obj_set_flex_flow(scr_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr_settings, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scr_settings, 8, 0);
    lv_obj_set_style_pad_ver(scr_settings, 10, 0);

    settings_info = make_label(scr_settings, "", &lv_font_montserrat_14, MUTED);

    auto add_button = [](const char *label, lv_event_cb_t cb) {
        lv_obj_t *btn = lv_button_create(scr_settings);
        lv_obj_set_width(btn, 240);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    };

    add_button(LV_SYMBOL_LIST " Choose printer", [](lv_event_t *) { show_printers(); });
    add_button(LV_SYMBOL_EDIT " Enter printer address", [](lv_event_t *) { show_host_screen(); });
    add_button(LV_SYMBOL_WIFI " Change Wi-Fi network", [](lv_event_t *) {
        WiFi.disconnect();
        prefs.remove("ssid");
        prefs.remove("pass");
        show_wifi_list();
    });
    add_button(LV_SYMBOL_SETTINGS " Recalibrate touch", [](lv_event_t *) {
        Preferences p;
        p.begin("touch", false);
        p.remove("cal5");
        p.end();
        ESP.restart();
    });
    add_button(LV_SYMBOL_LEFT " Back", [](lv_event_t *) { menu_show(); });
}

// ---------------------------------------------------------------- printer list

void select_printer(const FoundPrinter &p) {
    printer_host = p.ip.toString();
    prefs.putString("host", printer_host);
    moonraker_select_printer(printer_host, p.mac);
    camera_set_host(printer_host);
    show_dashboard();
}

void refresh_printers(lv_timer_t *) {
    if (lv_screen_active() != scr_printers) {
        lv_timer_delete(printers_timer);
        printers_timer = nullptr;
        return;
    }
    int n = printer_scan_results(found_printers, PRINTER_SCAN_MAX);
    bool running = printer_scan_running();
    char prog[32];
    printer_scan_progress(prog, sizeof(prog));
    int seen = 0;
    for (int i = 0; i < n; i++) seen += found_printers[i].seen;
    if (running)
        lv_label_set_text(printers_status, prog);
    else if (n == 0)
        lv_label_set_text(printers_status, "No printers found on this network");
    else if (seen == n)
        lv_label_set_text_fmt(printers_status, "%d found", n);
    else
        lv_label_set_text_fmt(printers_status, "%d found, %d not seen this scan", seen, n - seen);

    // Entries change in place (new IP, seen this scan), so compare contents.
    String current = moonraker_printer_mac();
    uint32_t sig = 2166136261u ^ (running ? 1 : 0) ^ (n << 1);
    auto mix = [&sig](const void *p, size_t len) {
        for (size_t i = 0; i < len; i++) sig = (sig ^ static_cast<const uint8_t *>(p)[i]) * 16777619u;
    };
    mix(current.c_str(), current.length());
    for (int i = 0; i < n; i++) {
        uint32_t ip = (uint32_t)found_printers[i].ip;
        mix(&ip, 4);
        mix(found_printers[i].name, strlen(found_printers[i].name));
        mix(&found_printers[i].seen, 1);
    }
    if (sig == 0) sig = 1;
    if (sig == shown_printers) return;
    shown_printers = sig;

    lv_obj_clean(printer_list);
    for (int i = 0; i < n; i++) {
        const FoundPrinter &p = found_printers[i];
        char line[72];
        snprintf(line, sizeof(line), "%s  %s%s", p.name[0] ? p.name : "Moonraker", p.ip.toString().c_str(),
                 p.seen || running ? "" : "  (not seen)");
        bool is_current = current == lan_mac_to_string(p.mac);
        lv_obj_t *btn = lv_list_add_button(printer_list, is_current ? LV_SYMBOL_OK : LV_SYMBOL_HOME, line);
        // Results only ever get appended, so the index stays valid.
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t *e) {
                int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
                select_printer(found_printers[idx]);
            },
            LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    if (n == 0 && running) lv_list_add_text(printer_list, "Searching...");
}

void build_printers_screen() {
    scr_printers = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_printers, BG, 0);
    lv_obj_remove_flag(scr_printers, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = make_icon_button(scr_printers, LV_SYMBOL_LEFT, 44, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_add_event_cb(back, [](lv_event_t *) { show_settings(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = make_label(scr_printers, "Choose printer", &lv_font_montserrat_18, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 58, 9);

    lv_obj_t *rescan = lv_button_create(scr_printers);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_t *rescan_lbl = lv_label_create(rescan);
    lv_label_set_text(rescan_lbl, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_add_event_cb(
        rescan,
        [](lv_event_t *) {
            printer_scan_start();
            shown_printers = 0;
        },
        LV_EVENT_CLICKED, NULL);

    printers_status = make_label(scr_printers, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(printers_status, LV_ALIGN_TOP_LEFT, 10, 42);

    printer_list = lv_list_create(scr_printers);
    lv_obj_set_size(printer_list, SCREEN_WIDTH - 20, SCREEN_HEIGHT - 72);
    lv_obj_align(printer_list, LV_ALIGN_BOTTOM_MID, 0, -6);
}

// ---------------------------------------------------------------- navigation

void show_wifi_list() {
    load_screen(scr_list);
    lv_obj_clean(list_widget);
    lv_list_add_text(list_widget, "Scanning...");
    lv_refr_now(nullptr); // force this to actually show before the blocking scan below

    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    delay(100);
    int n = WiFi.scanNetworks(); // blocking: async scan was unreliable on the CYD
    populate_list(n);
}

void show_password_screen(const String &ssid) {
    entry_mode = EntryMode::WifiPassword;
    selected_ssid = ssid;
    lv_label_set_text_fmt(entry_title, "Password for %s", ssid.c_str());
    lv_textarea_set_password_mode(entry_ta, false); // visible: easier to spot typos
    lv_textarea_set_text(entry_ta, "");
    load_screen(scr_entry);
}

void show_host_screen() {
    entry_mode = EntryMode::PrinterHost;
    lv_label_set_text(entry_title, "Moonraker host (ip, ip:port or MAC)");
    lv_textarea_set_text(entry_ta, printer_host.c_str());
    load_screen(scr_entry);
}

void show_connecting(const String &ssid, const String &pass) {
    selected_ssid = ssid;
    pending_password = pass;
    lv_label_set_text_fmt(connecting_label, "Connecting to %s", ssid.c_str());
    load_screen(scr_connecting);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    connect_start_ms = millis();

    if (connect_poll_timer) lv_timer_delete(connect_poll_timer);
    connect_poll_timer = lv_timer_create(poll_connect, 300, NULL);
}

void show_dashboard() {
    camera_set_view(DASH_CAM_W, DASH_CAM_H);
    camera_set_active(true); // for the camera tile; it only shows once frames arrive
    if (!moonraker_started) {
        moonraker_start(printer_host, DEFAULT_PRINTER_MAC);
        moonraker_started = true;
    }
    update_dashboard(nullptr);
    load_screen(scr_dashboard);
    if (!dashboard_timer) dashboard_timer = lv_timer_create(update_dashboard, 500, NULL);
    static lv_timer_t *cam_timer = nullptr;
    if (!cam_timer) cam_timer = lv_timer_create(cam_tick, 40, NULL);
}

void show_camera() {
    camera_set_view(CAM_W, CAM_H);
    camera_set_active(true);
    load_screen(scr_camera);
    update_dashboard(nullptr);
}

void show_printers() {
    camera_set_active(false);
    // The remembered list shows right away; the scan refreshes it.
    printer_scan_start(); // no-op if one is still running
    shown_printers = 0;
    load_screen(scr_printers);
    refresh_printers(nullptr);
    if (!printers_timer) printers_timer = lv_timer_create(refresh_printers, 300, NULL);
}

void show_settings() {
    camera_set_active(false);
    String mac = moonraker_printer_mac();
    lv_label_set_text_fmt(settings_info, "Me: %s   Printer: %s\nPrinter MAC: %s",
                          WiFi.localIP().toString().c_str(), printer_host.c_str(),
                          mac.length() ? mac.c_str() : "?");
    load_screen(scr_settings);
}

} // namespace

void ui_debug_command(char c) {
    switch (c) {
    case 'd': show_dashboard(); break;
    case 'c': show_camera(); break;
    case 'g': show_settings(); break;
    case 'p': show_printers(); break;
    case 's': display_request_screenshot(); break;
    case 'l': {
        FoundPrinter list[PRINTER_SCAN_MAX];
        int n = printer_scan_results(list, PRINTER_SCAN_MAX);
        for (int i = 0; i < n; i++)
            Serial.printf("[list] %d: %s %s %s%s\n", i + 1, list[i].name, list[i].ip.toString().c_str(),
                          lan_mac_to_string(list[i].mac).c_str(), list[i].seen ? "" : " (not seen)");
        break;
    }
    default:
        menu_debug(c);
        if (c >= '1' && c <= '9') {
            FoundPrinter list[PRINTER_SCAN_MAX];
            int n = printer_scan_results(list, PRINTER_SCAN_MAX);
            if (c - '1' < n) select_printer(list[c - '1']);
        }
        break;
    }
}

void ui_init() {
    prefs.begin("printmon", false);
    printer_host = prefs.getString("host", DEFAULT_PRINTER_HOST);
    printer_scan_results(nullptr, 0); // loads the remembered printer list
    camera_set_host(printer_host);

    build_wifi_list_screen();
    build_entry_screen();
    build_connecting_screen();
    build_dashboard_screen();
    build_camera_screen();
    build_settings_screen();
    build_printers_screen();

    // Reuse snekdisplay's saved Wi-Fi if this is the same board.
    String saved_ssid = prefs.getString("ssid", "");
    String saved_pass = prefs.getString("pass", "");
    if (saved_ssid.isEmpty()) {
        Preferences old;
        old.begin("wifi", true);
        saved_ssid = old.getString("ssid", "");
        saved_pass = old.getString("pass", "");
        old.end();
    }
    if (saved_ssid.length() > 0) {
        show_connecting(saved_ssid, saved_pass);
    } else {
        show_wifi_list();
    }
}

// ---------------------------------------------------------------- shared with menu.cpp

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color) {
    return make_label(parent, text, font, color);
}
void ui_style_button(lv_obj_t *btn, lv_color_t color) { style_button(btn, color); }
void ui_set_text(lv_obj_t *label, const char *text) { set_text(label, text); }
void ui_load(lv_obj_t *scr) { load_screen(scr); }
void ui_load_temp(lv_obj_t *scr) {
    load_screen(scr);
    temp_screen = scr;
}
void ui_show_dashboard() { show_dashboard(); }
void ui_show_settings() { show_settings(); }
bool ui_current_notice(char *buf, size_t n) { return current_notice(buf, n); }
