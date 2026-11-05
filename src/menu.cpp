// The ☰ menu: everything Mainsail does that isn't on the home screen.
// Every screen here is built when it's opened and deleted when you leave
// (ui_load_temp), so none of it costs RAM while you're on the dashboard.

#include <Arduino.h>
#include <lvgl.h>

#include "camera.h"
#include "display.h"
#include "moonraker.h"
#include "printer_api.h"
#include "ui_common.h"

namespace {

// ---------------------------------------------------------------- frame

lv_obj_t *cur_scr = nullptr;
lv_obj_t *notice_lbl = nullptr;
char idle_text[64] = ""; // shown in the bottom line when there's no notice
void (*refresh_fn)(const PrinterStatus &) = nullptr;
lv_timer_t *timer = nullptr;

void tick(lv_timer_t *) {
    if (!cur_scr || lv_screen_active() != cur_scr) return;
    PrinterStatus s;
    moonraker_get(s);
    if (refresh_fn) refresh_fn(s);
    char buf[64];
    if (ui_current_notice(buf, sizeof(buf)))
        ui_set_text(notice_lbl, buf);
    else
        ui_set_text(notice_lbl, idle_text);
}

// A screen with a back button, a title and a status line at the bottom.
lv_obj_t *new_screen(const char *title, lv_event_cb_t back, void (*refresh)(const PrinterStatus &) = nullptr) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 44, 32);
    lv_obj_set_pos(b, 6, 4);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = ui_label(scr, title, &lv_font_montserrat_18, lv_color_white());
    lv_obj_set_pos(t, 58, 9);

    notice_lbl = ui_label(scr, "", &lv_font_montserrat_14, MUTED);
    lv_label_set_long_mode(notice_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(notice_lbl, SCREEN_WIDTH - 16);
    lv_obj_set_pos(notice_lbl, 8, SCREEN_HEIGHT - 18);

    cur_scr = scr;
    refresh_fn = refresh;
    camera_set_active(false); // its downloads and decoding compete for RAM and CPU
    idle_text[0] = 0;
    if (!timer) timer = lv_timer_create(tick, 300, NULL);
    return scr;
}

void open(lv_obj_t *scr) {
    ui_load_temp(scr);
    tick(nullptr);
}

lv_obj_t *button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, lv_event_cb_t cb,
                 intptr_t data = 0) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
    return b;
}

intptr_t data_of(lv_event_t *e) {
    return reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
}

bool printing(const PrinterStatus &s) {
    return !strcmp(s.state, "printing") || !strcmp(s.state, "paused");
}

void enable(lv_obj_t *obj, bool on) {
    if (on == lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        if (on)
            lv_obj_remove_state(obj, LV_STATE_DISABLED);
        else
            lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

void back_to_menu(lv_event_t *) { menu_show(); }

// A value the user is changing: taps add up right away, before the next
// status poll (2 s) reports the new value back.
struct Pending {
    float value = 0;
    uint32_t ms = 0;
    float get(float reported) const { return ms && millis() - ms < 4000 ? value : reported; }
    void set(float v) {
        value = v;
        ms = millis();
    }
};

// ---------------------------------------------------------------- confirm

// Anything that starts, stops or reboots something asks first.
struct Confirmed {
    bool gcode; // else POST path
    String arg;
    String label;
    void (*back)();
};
Confirmed pending_action;

void confirm(const String &question, lv_color_t color, const char *ok_text, bool gcode, const String &arg,
             const char *label, void (*back)()) {
    pending_action = {gcode, arg, label, back};
    lv_obj_t *scr = new_screen("Are you sure?", [](lv_event_t *) { pending_action.back(); });
    lv_obj_t *q = ui_label(scr, question.c_str(), &lv_font_montserrat_18, lv_color_white());
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(q, SCREEN_WIDTH - 32);
    lv_obj_set_pos(q, 16, 56);

    button(scr, "Cancel", 16, 160, 136, 48, [](lv_event_t *) { pending_action.back(); });
    lv_obj_t *ok = button(scr, ok_text, 168, 160, 136, 48, [](lv_event_t *) {
        if (pending_action.gcode)
            api_gcode(pending_action.arg, pending_action.label.c_str());
        else
            api_post(pending_action.arg, pending_action.label.c_str());
        pending_action.back();
    });
    ui_style_button(ok, color);
    open(scr);
}

String url_encode(const char *s) {
    String out;
    const char *hex = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ---------------------------------------------------------------- temperatures

struct Heater {
    const char *name;    // Klipper heater
    const char *caption;
    int max;
    lv_obj_t *label;
    Pending target;
};
Heater heaters[2] = {{"extruder", "Nozzle", 300, nullptr, {}}, {"heater_bed", "Bed", 120, nullptr, {}}};

void set_target(int h, float t) {
    Heater &ht = heaters[h];
    t = constrain(t, 0.0f, (float)ht.max);
    ht.target.set(t);
    char g[64], l[32];
    snprintf(g, sizeof(g), "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%.0f", ht.name, t);
    snprintf(l, sizeof(l), "%s %.0f\xC2\xB0", ht.caption, t);
    api_gcode(g, l);
}

float shown_target(int h, const PrinterStatus &s) {
    return heaters[h].target.get(h == 0 ? s.extruder_target : s.bed_target);
}

void refresh_temps(const PrinterStatus &s) {
    for (int h = 0; h < 2; h++) {
        char buf[48];
        float cur = h == 0 ? s.extruder_temp : s.bed_temp;
        float tgt = shown_target(h, s);
        if (tgt > 0)
            snprintf(buf, sizeof(buf), "%s  %.1f\xC2\xB0 / %.0f\xC2\xB0", heaters[h].caption, cur, tgt);
        else
            snprintf(buf, sizeof(buf), "%s  %.1f\xC2\xB0  (off)", heaters[h].caption, cur);
        ui_set_text(heaters[h].label, buf);
    }
}

void show_temps() {
    lv_obj_t *scr = new_screen("Temperatures", back_to_menu, refresh_temps);
    static const int deltas[] = {-10, -1, 1, 10};
    for (int h = 0; h < 2; h++) {
        int y = 44 + h * 62;
        heaters[h].label = ui_label(scr, "", &lv_font_montserrat_18, lv_color_white());
        lv_obj_set_pos(heaters[h].label, 10, y);
        button(scr, "Off", 8, y + 24, 56, 34, [](lv_event_t *e) { set_target(data_of(e), 0); }, h);
        for (int i = 0; i < 4; i++) {
            char t[8];
            snprintf(t, sizeof(t), "%+d", deltas[i]);
            // user data: heater in bit 8, delta index below
            button(scr, t, 72 + i * 60, y + 24, 54, 34, [](lv_event_t *e) {
                int d = data_of(e);
                int h = d >> 8;
                PrinterStatus s;
                moonraker_get(s);
                set_target(h, shown_target(h, s) + deltas[d & 0xff]);
            }, (h << 8) | i);
        }
    }
    // Presets: nozzle / bed.
    struct Preset {
        const char *name;
        int nozzle, bed;
    };
    static const Preset presets[] = {{"PLA", 210, 60}, {"PETG", 240, 80}, {"ABS", 250, 100}, {"Cool", 0, 0}};
    for (int i = 0; i < 4; i++) {
        char t[24];
        if (presets[i].nozzle)
            snprintf(t, sizeof(t), "%s\n%d/%d", presets[i].name, presets[i].nozzle, presets[i].bed);
        else
            snprintf(t, sizeof(t), "%s\nall off", presets[i].name);
        lv_obj_t *b = button(scr, t, 8 + i * 77, 172, 72, 44, [](lv_event_t *e) {
            const Preset &p = presets[data_of(e)];
            set_target(0, p.nozzle);
            set_target(1, p.bed);
        }, i);
        lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_CENTER, 0);
        if (!presets[i].nozzle) ui_style_button(b, CARD_BG);
    }
    open(scr);
}

// ---------------------------------------------------------------- move

float jog_step = 10;
lv_obj_t *step_btns[3];
lv_obj_t *move_btns[12];
int move_btn_count = 0;

void refresh_move(const PrinterStatus &s) {
    bool busy = printing(s);
    for (int i = 0; i < move_btn_count; i++) enable(move_btns[i], !busy && s.reachable);
    if (busy)
        snprintf(idle_text, sizeof(idle_text), "Moving is disabled while printing");
    else
        snprintf(idle_text, sizeof(idle_text), "X %.1f  Y %.1f  Z %.2f   homed: %s", s.pos[0], s.pos[1], s.pos[2],
                 s.homed_axes[0] ? s.homed_axes : "none");
}

void style_steps() {
    static const float steps[] = {0.1f, 1, 10};
    for (int i = 0; i < 3; i++) ui_style_button(step_btns[i], steps[i] == jog_step ? ACCENT : CARD_BG);
}

void jog(char axis, int dir) {
    char g[64], l[24];
    float feed = axis == 'Z' ? 600 : 6000;
    snprintf(g, sizeof(g), "G91\nG1 %c%.2f F%.0f\nG90", axis, dir * jog_step, feed);
    snprintf(l, sizeof(l), "%c%+.1f", axis, dir * jog_step);
    api_gcode(g, l);
}

void show_move() {
    lv_obj_t *scr = new_screen("Move", back_to_menu, refresh_move);
    move_btn_count = 0;
    auto add = [](lv_obj_t *b) { move_btns[move_btn_count++] = b; };
    // XY cross
    const int cx = 8, cy = 44, s = 50;
    add(button(scr, "Y+", cx + s, cy, s - 4, s - 4, [](lv_event_t *) { jog('Y', 1); }));
    add(button(scr, "X-", cx, cy + s, s - 4, s - 4, [](lv_event_t *) { jog('X', -1); }));
    add(button(scr, "X+", cx + 2 * s, cy + s, s - 4, s - 4, [](lv_event_t *) { jog('X', 1); }));
    add(button(scr, "Y-", cx + s, cy + 2 * s, s - 4, s - 4, [](lv_event_t *) { jog('Y', -1); }));
    add(button(scr, LV_SYMBOL_HOME, cx + s, cy + s, s - 4, s - 4,
               [](lv_event_t *) { api_gcode("G28 X Y", "Home XY"); }));
    // Z
    add(button(scr, "Z+", 166, cy, 52, 70, [](lv_event_t *) { jog('Z', 1); }));
    add(button(scr, "Z-", 166, cy + 76, 52, 70, [](lv_event_t *) { jog('Z', -1); }));
    // Right column
    add(button(scr, LV_SYMBOL_HOME " All", 226, cy, 88, 46, [](lv_event_t *) { api_gcode("G28", "Home all"); }));
    add(button(scr, LV_SYMBOL_HOME " Z", 226, cy + 50, 88, 46, [](lv_event_t *) { api_gcode("G28 Z", "Home Z"); }));
    lv_obj_t *off = button(scr, "Motors\noff", 226, cy + 100, 88, 46,
                           [](lv_event_t *) { api_gcode("M84", "Motors off"); });
    lv_obj_set_style_text_align(lv_obj_get_child(off, 0), LV_TEXT_ALIGN_CENTER, 0);
    ui_style_button(off, CARD_BG);
    add(off);
    // Step size
    static const char *labels[] = {"0.1 mm", "1 mm", "10 mm"};
    for (int i = 0; i < 3; i++) {
        step_btns[i] = button(scr, labels[i], 8 + i * 70, 196, 64, 24, [](lv_event_t *e) {
            static const float steps[] = {0.1f, 1, 10};
            jog_step = steps[data_of(e)];
            style_steps();
        }, i);
    }
    style_steps();
    open(scr);
}

// ---------------------------------------------------------------- tune

enum TuneRow { Fan, Speed, Flow, ZOffset, TUNE_ROWS };
Pending tune_pending[TUNE_ROWS];
lv_obj_t *tune_values[TUNE_ROWS];

float tune_reported(int row, const PrinterStatus &s) {
    switch (row) {
    case Fan: return s.fan_speed * 100;
    case Speed: return s.speed_factor * 100;
    case Flow: return s.extrude_factor * 100;
    default: return s.z_offset;
    }
}

void refresh_tune(const PrinterStatus &s) {
    for (int r = 0; r < TUNE_ROWS; r++) {
        char buf[16];
        float v = tune_pending[r].get(tune_reported(r, s));
        if (r == ZOffset)
            snprintf(buf, sizeof(buf), "%+.3f", v);
        else
            snprintf(buf, sizeof(buf), "%.0f %%", v);
        ui_set_text(tune_values[r], buf);
    }
}

void tune_step(int row, int dir) {
    static const float steps[] = {10, 10, 1, 0.025f};
    PrinterStatus s;
    moonraker_get(s);
    float cur = tune_pending[row].get(tune_reported(row, s));
    float step = steps[row] * dir;
    char g[64], l[24];
    float v;
    switch (row) {
    case Fan:
        v = constrain(cur + step, 0.0f, 100.0f);
        snprintf(g, sizeof(g), "M106 S%d", (int)lroundf(v * 2.55f));
        snprintf(l, sizeof(l), "Fan %.0f%%", v);
        break;
    case Speed:
        v = constrain(cur + step, 10.0f, 300.0f);
        snprintf(g, sizeof(g), "M220 S%.0f", v);
        snprintf(l, sizeof(l), "Speed %.0f%%", v);
        break;
    case Flow:
        v = constrain(cur + step, 50.0f, 150.0f);
        snprintf(g, sizeof(g), "M221 S%.0f", v);
        snprintf(l, sizeof(l), "Flow %.0f%%", v);
        break;
    default:
        // Babystepping: relative, and applied right away (MOVE=1).
        v = cur + step;
        snprintf(g, sizeof(g), "SET_GCODE_OFFSET Z_ADJUST=%+.3f MOVE=1", step);
        snprintf(l, sizeof(l), "Z offset %+.3f", v);
        break;
    }
    tune_pending[row].set(v);
    api_gcode(g, l);
}

void show_tune() {
    lv_obj_t *scr = new_screen("Tune", back_to_menu, refresh_tune);
    static const char *captions[] = {"Part fan", "Speed", "Flow", "Z mm"};
    static const char *minus[] = {"-10", "-10", "-1", "-.025"};
    static const char *plus[] = {"+10", "+10", "+1", "+.025"};
    for (int r = 0; r < TUNE_ROWS; r++) {
        int y = 44 + r * 44;
        lv_obj_t *c = ui_label(scr, captions[r], &lv_font_montserrat_18, lv_color_white());
        lv_obj_set_pos(c, 10, y + 9);
        button(scr, minus[r], 118, y, 62, 38, [](lv_event_t *e) { tune_step(data_of(e), -1); }, r);
        tune_values[r] = ui_label(scr, "", &lv_font_montserrat_18, lv_color_white());
        lv_obj_set_width(tune_values[r], 70);
        lv_obj_set_style_text_align(tune_values[r], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(tune_values[r], 182, y + 9);
        button(scr, plus[r], 254, y, 60, 38, [](lv_event_t *e) { tune_step(data_of(e), 1); }, r);
    }
    open(scr);
}

// ---------------------------------------------------------------- macros & files

ApiList list_kind;
lv_obj_t *list_obj;
uint32_t list_gen = UINT32_MAX;
bool list_was_loading = false;

void show_list(ApiList kind);

void open_item(int i) {
    char buf[API_NAME_LEN];
    if (!api_list_name(list_kind, i, buf, sizeof(buf))) return;
    String name = buf;
    if (list_kind == ApiList::Macros) {
        confirm("Run macro " + name + "?", ACCENT, LV_SYMBOL_PLAY " Run", true, name, name.c_str(),
                [] { show_list(ApiList::Macros); });
    } else {
        int slash = name.lastIndexOf('/');
        String shown = slash >= 0 ? name.substring(slash + 1) : name;
        confirm("Start printing " + shown + "?", GOOD, LV_SYMBOL_PLAY " Print", false,
                "/printer/print/start?filename=" + url_encode(name.c_str()), "Start print",
                [] { ui_show_dashboard(); });
    }
}

void refresh_list(const PrinterStatus &) {
    bool loading;
    uint32_t gen;
    int n = api_list(list_kind, &loading, &gen);
    if (loading)
        snprintf(idle_text, sizeof(idle_text), "Loading...");
    else
        snprintf(idle_text, sizeof(idle_text), "%d %s", n,
                 list_kind == ApiList::Files ? "newest files" : "macros");
    if (gen == list_gen && loading == list_was_loading) return;
    list_gen = gen;
    list_was_loading = loading;
    lv_obj_clean(list_obj);
    for (int i = 0; i < n; i++) {
        char name[API_NAME_LEN];
        if (!api_list_name(list_kind, i, name, sizeof(name))) break;
        const char *slash = strrchr(name, '/');
        // A plain button + one label: lv_list's rows (button, image, label,
        // flex layout, scrolling text) cost ~750 bytes each.
        lv_obj_t *b = lv_button_create(list_obj);
        lv_obj_set_size(b, lv_pct(100), 34);
        ui_style_button(b, CARD_BG);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_text_fmt(l, "%s  %s", list_kind == ApiList::Files ? LV_SYMBOL_FILE : LV_SYMBOL_PLAY,
                              slash ? slash + 1 : name);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(b, [](lv_event_t *e) { open_item(data_of(e)); }, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    if (n == 0 && !loading) {
        lv_obj_t *l = ui_label(list_obj, list_kind == ApiList::Files ? "No files" : "No macros",
                               &lv_font_montserrat_14, MUTED);
        (void)l;
    }
}

void show_list(ApiList kind) {
    list_kind = kind;
    list_gen = UINT32_MAX;
    lv_obj_t *scr = new_screen(kind == ApiList::Files ? "Print a file" : "Macros", back_to_menu, refresh_list);
    lv_obj_t *re = button(scr, LV_SYMBOL_REFRESH, SCREEN_WIDTH - 50, 4, 44, 32,
                          [](lv_event_t *) { api_fetch(list_kind); });
    (void)re;
    list_obj = lv_obj_create(scr);
    lv_obj_set_size(list_obj, SCREEN_WIDTH - 12, SCREEN_HEIGHT - 64);
    lv_obj_set_pos(list_obj, 6, 42);
    lv_obj_set_style_bg_opa(list_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_obj, 0, 0);
    lv_obj_set_style_pad_all(list_obj, 0, 0);
    lv_obj_set_style_pad_row(list_obj, 4, 0);
    lv_obj_set_flex_flow(list_obj, LV_FLEX_FLOW_COLUMN);
    api_fetch(kind); // shows what it has, then refreshes
    open(scr);
}

// ---------------------------------------------------------------- power

void show_power() {
    lv_obj_t *scr = new_screen("Power & restarts", back_to_menu);
    struct Item {
        const char *text;
        const char *question;
        const char *path;
        const char *label;
        bool danger;
    };
    static const Item items[] = {
        {LV_SYMBOL_REFRESH " Restart Klipper", "Restart Klipper?", "/printer/restart", "Klipper restart", false},
        {LV_SYMBOL_REFRESH " Firmware restart", "Restart the printer's firmware?", "/printer/firmware_restart",
         "Firmware restart", false},
        {LV_SYMBOL_REFRESH " Restart Moonraker", "Restart Moonraker?", "/server/restart", "Moonraker restart",
         false},
        {LV_SYMBOL_POWER " Reboot host", "Reboot the printer's computer?", "/machine/reboot", "Reboot", true},
        {LV_SYMBOL_POWER " Shut down host", "Shut down the printer's computer? You'll have to power it on again.",
         "/machine/shutdown", "Shutdown", true},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = button(scr, items[i].text, 40, 42 + i * 36, 240, 32, [](lv_event_t *e) {
            const Item &it = items[data_of(e)];
            confirm(it.question, it.danger ? BAD : WARN, "Yes", false, it.path, it.label, show_power);
        }, i);
        if (items[i].danger) ui_style_button(b, CARD_BG);
    }
    open(scr);
}

// ---------------------------------------------------------------- menu

lv_obj_t *cancel_btn;

void refresh_menu(const PrinterStatus &s) {
    enable(cancel_btn, printing(s));
    snprintf(idle_text, sizeof(idle_text), "%s", s.reachable ? moonraker_host().c_str() : "Printer offline");
}

} // namespace

void menu_show() {
    api_start();
    lv_obj_t *scr = new_screen("Menu", [](lv_event_t *) { ui_show_dashboard(); }, refresh_menu);
    struct Entry {
        const char *text;
        void (*fn)();
    };
    static const Entry entries[] = {
        {LV_SYMBOL_CHARGE " Temperatures", show_temps},
        {LV_SYMBOL_GPS " Move", show_move},
        {LV_SYMBOL_SETTINGS " Tune", show_tune},
        {LV_SYMBOL_PLAY " Macros", [] { show_list(ApiList::Macros); }},
        {LV_SYMBOL_FILE " Print a file", [] { show_list(ApiList::Files); }},
        {LV_SYMBOL_CLOSE " Cancel print", nullptr},
        {LV_SYMBOL_POWER " Power", show_power},
        {LV_SYMBOL_EDIT " Display", ui_show_settings},
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *b = button(scr, entries[i].text, 8 + (i % 2) * 156, 42 + (i / 2) * 44, 148, 40, [](lv_event_t *e) {
            int i = data_of(e);
            if (entries[i].fn)
                entries[i].fn();
            else
                confirm("Cancel the current print?", BAD, "Cancel print", false, "/printer/print/cancel",
                        "Cancel print", [] { ui_show_dashboard(); });
        }, i);
        if (!entries[i].fn) {
            ui_style_button(b, BAD);
            cancel_btn = b;
        }
    }
    open(scr);
}

void menu_debug(char c) {
    switch (c) {
    case 'm': menu_show(); break;
    case 't': show_temps(); break;
    case 'v': show_move(); break;
    case 'u': show_tune(); break;
    case 'k': show_list(ApiList::Macros); break;
    case 'f': show_list(ApiList::Files); break;
    case 'w': show_power(); break;
    default: break;
    }
}
