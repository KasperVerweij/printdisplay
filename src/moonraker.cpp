#include "moonraker.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include "lan_finder.h"
#include "printer_scan.h"

namespace {

constexpr uint32_t POLL_INTERVAL_MS = 2000;

SemaphoreHandle_t lock;
PrinterStatus shared;
String host = "";     // guarded by lock
uint32_t host_gen = 0; // bumped on host change so the task drops cached metadata

// Printer identity on the LAN (see lan_finder.h). Guarded by lock.
uint8_t printer_mac[6];
bool have_mac = false;
bool relearn_mac = false; // user picked a host: trust whatever answers there
bool force_search = false; // user entered a MAC: go look for it now
bool mac_saved = false;    // in NVS (not just the build-time default)

constexpr int FAILS_BEFORE_SEARCH = 3;            // ~6 s of no answer
constexpr uint32_t SEARCH_INTERVAL_MS = 60000;   // between scans while lost

QueueHandle_t cmd_queue;
TaskHandle_t estop_task_handle = nullptr;
char cmd_result[48] = ""; // guarded by lock
uint32_t cmd_result_ms = 0;

void copy_str(char *dst, size_t n, const char *src) {
    strlcpy(dst, src ? src : "", n);
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

String base_url(const String &h) {
    return h.indexOf(':') >= 0 ? "http://" + h : "http://" + h + ":7125";
}

void set_result(const char *text) {
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(cmd_result, text, sizeof(cmd_result));
    cmd_result_ms = millis();
    xSemaphoreGive(lock);
}

void run_command(const String &base, PrinterCommand cmd) {
    const char *path = "", *name = "";
    switch (cmd) {
    case PrinterCommand::Pause: path = "/printer/print/pause"; name = "Pause"; break;
    case PrinterCommand::Resume: path = "/printer/print/resume"; name = "Resume"; break;
    case PrinterCommand::EmergencyStop: return; // handled by estop_task
    case PrinterCommand::FirmwareRestart: path = "/printer/firmware_restart"; name = "Firmware restart"; break;
    case PrinterCommand::KlipperRestart: path = "/printer/restart"; name = "Klipper restart"; break;
    }
    HTTPClient http;
    http.setConnectTimeout(1500);
    // Pause/resume return only once the macro finishes (can take a while).
    http.setTimeout(15000);
    int code = -1;
    if (http.begin(base + path)) {
        code = http.POST("");
        http.end();
    }
    char buf[48];
    if (code == 200)
        snprintf(buf, sizeof(buf), "%s sent", name);
    else
        snprintf(buf, sizeof(buf), "%s failed (%d)", name, code);
    set_result(buf);
}

int post(const String &url, uint16_t timeout_ms) {
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(timeout_ms);
    int code = -1;
    if (http.begin(url)) {
        code = http.POST("");
        http.end();
    }
    return code;
}

// E-stop runs in its own task with its own connection, so it never waits
// behind a poll, a metadata fetch or a pause macro (up to 15 s) in the
// polling task. It retries until Moonraker confirms.
void estop_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        String base;
        xSemaphoreTake(lock, portMAX_DELAY);
        base = base_url(host);
        xSemaphoreGive(lock);

        int code = -1;
        for (int attempt = 1; attempt <= 6 && code != 200; attempt++) {
            if (WiFi.status() == WL_CONNECTED) code = post(base + "/printer/emergency_stop", 3000);
            Serial.printf("[estop] attempt %d -> %d\n", attempt, code);
            if (code == 200) break;
            char buf[48];
            snprintf(buf, sizeof(buf), "E-STOP failed (%d), retrying", code);
            set_result(buf);
            // A new press while retrying just restarts the loop sooner.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        }
        if (code == 200) {
            set_result("EMERGENCY STOP sent");
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "E-STOP FAILED (%d)!", code);
            set_result(buf);
        }
    }
}

bool http_get_json(const String &url, JsonDocument &doc, JsonDocument &filter, int *code_out = nullptr) {
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    http.useHTTP10(true); // never chunked: we parse straight off the raw stream
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code_out) *code_out = code;
    bool ok = false;
    if (code == 200) {
        ok = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) ==
             DeserializationError::Ok;
    }
    http.end();
    return ok;
}

// Per-file metadata (slicer time estimate, layer geometry) — fetched once
// whenever the active filename changes.
struct FileMeta {
    String filename;
    float estimated_time = 0;
    float filament_total = 0;
    float first_layer_height = 0;
    float layer_height = 0;
    int layer_count = -1;
};

void fetch_metadata(const String &base, const char *filename, FileMeta &meta) {
    meta = FileMeta();
    meta.filename = filename;
    if (!*filename) return;

    JsonDocument filter;
    JsonObject r = filter["result"].to<JsonObject>();
    r["estimated_time"] = true;
    r["filament_total"] = true;
    r["first_layer_height"] = true;
    r["layer_height"] = true;
    r["layer_count"] = true;
    r["object_height"] = true;

    JsonDocument doc;
    if (!http_get_json(base + "/server/files/metadata?filename=" + url_encode(filename), doc, filter)) return;
    JsonObject res = doc["result"];
    meta.estimated_time = res["estimated_time"] | 0.0f;
    meta.filament_total = res["filament_total"] | 0.0f;
    meta.first_layer_height = res["first_layer_height"] | 0.0f;
    meta.layer_height = res["layer_height"] | 0.0f;
    meta.layer_count = res["layer_count"] | -1;
    float object_height = res["object_height"] | 0.0f;
    if (meta.layer_count < 0 && meta.layer_height > 0 && object_height > 0)
        meta.layer_count = (int)roundf((object_height - meta.first_layer_height) / meta.layer_height) + 1;
}

void save_mac(const uint8_t mac[6]) {
    Preferences p;
    p.begin("printmon", false);
    p.putString("mac", lan_mac_to_string(mac));
    p.end();
}

// "192.168.0.67:7125" -> IP + ":7125". False for hostnames and non-LAN IPs
// (other subnets): MAC lookups only work on our own subnet.
bool split_lan_host(const String &h, IPAddress &ip, String &port_suffix) {
    int colon = h.indexOf(':');
    String addr = colon >= 0 ? h.substring(0, colon) : h;
    port_suffix = colon >= 0 ? h.substring(colon) : "";
    return ip.fromString(addr) && lan_in_subnet(ip);
}

void set_searching(bool on) {
    xSemaphoreTake(lock, portMAX_DELAY);
    shared.searching = on;
    xSemaphoreGive(lock);
}

// Points this display's ARP entry for a LAN host at the printer's MAC, so
// an IP conflict can't send our traffic to the other device.
void pin_host(const String &h) {
    IPAddress ip;
    String port;
    uint8_t mac[6];
    xSemaphoreTake(lock, portMAX_DELAY);
    bool known = have_mac;
    memcpy(mac, printer_mac, 6);
    xSemaphoreGive(lock);
    if (known && split_lan_host(h, ip, port))
        lan_pin(ip, mac);
    else
        lan_pin(IPAddress(), nullptr);
}

// Scans the LAN for the printer's MAC. Returns true if the host moved.
// Unless forced (the user just entered the MAC), only for LAN hosts: a
// hostname or other-subnet address that isn't answering is left alone.
bool search_printer(const String &h, bool forced) {
    uint8_t mac[6];
    xSemaphoreTake(lock, portMAX_DELAY);
    bool known = have_mac;
    memcpy(mac, printer_mac, 6);
    xSemaphoreGive(lock);
    if (!known) return false;

    IPAddress cur, found;
    String port;
    bool lan_host = split_lan_host(h, cur, port);
    if (!lan_host && !forced) return false;

    Serial.printf("[lan] looking for printer %s\n", lan_mac_to_string(mac).c_str());
    set_searching(true);
    // Give up as soon as the user picks another printer.
    uint32_t gen = host_gen;
    bool ok = printer_find(mac, found, [gen] { return host_gen != gen; });
    set_searching(false);
    if (!ok) {
        Serial.println("[lan] printer not found on this subnet");
        return false;
    }
    String nh = found.toString() + port;
    Serial.printf("[lan] printer is at %s\n", nh.c_str());
    if (nh == h) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    host = nh;
    host_gen++;
    xSemaphoreGive(lock);
    return true;
}

// After a successful poll: make sure our printer answered, not a different
// one that got its IP. Moonraker reports its own MACs, which (unlike ARP)
// can't be confused by another device with the same IP. Learns the MAC if
// we don't know it yet. Returns false if a different printer answered.
bool check_identity(const String &h) {
    IPAddress ip;
    String port;
    if (!split_lan_host(h, ip, port)) return true; // hostname/other subnet: can't check, trust it
    PrinterId id;
    if (!printer_identity(base_url(h), ip, id)) return true; // can't tell right now; try again later
    const uint8_t *at_ip = id.macs[id.at_ip >= 0 ? id.at_ip : 0];
    xSemaphoreTake(lock, portMAX_DELAY);
    bool learn = !have_mac || relearn_mac;
    bool same = have_mac && id.has(printer_mac);
    // Keep the MAC of the interface that has this IP: that's what ARP pins.
    bool changed = (learn || same) && (!have_mac || memcmp(printer_mac, at_ip, 6));
    if (changed) {
        memcpy(printer_mac, at_ip, 6);
        have_mac = true;
    }
    relearn_mac = false;
    bool save = changed || ((learn || same) && !mac_saved);
    mac_saved = mac_saved || save;
    xSemaphoreGive(lock);
    if (save) save_mac(at_ip);
    if (learn || same) {
        if (changed) Serial.printf("[lan] printer MAC %s at %s\n", lan_mac_to_string(at_ip).c_str(), h.c_str());
        pin_host(h);
        return true;
    }
    Serial.printf("[lan] %s is %s, not our printer\n", h.c_str(), lan_mac_to_string(at_ip).c_str());
    return false;
}

void poll_task(void *) {
    FileMeta meta;
    uint32_t seen_gen = UINT32_MAX; // != host_gen: set up (and pin) the first host
    bool identity_ok = false;  // checked for the current host
    int fails = 0;
    uint32_t last_search_ms = 0;
    bool searched = false;

    JsonDocument filter;
    JsonObject st = filter["result"]["status"].to<JsonObject>();
    st["webhooks"] = true;
    st["print_stats"] = true;
    st["display_status"]["progress"] = true;
    st["virtual_sdcard"]["progress"] = true;
    for (const char *h : {"extruder", "heater_bed"}) {
        st[h]["temperature"] = true;
        st[h]["target"] = true;
        st[h]["power"] = true;
    }
    st["toolhead"]["position"] = true;
    st["toolhead"]["homed_axes"] = true;
    st["fan"]["speed"] = true;
    st["gcode_move"]["speed_factor"] = true;
    st["gcode_move"]["extrude_factor"] = true;
    st["gcode_move"]["homing_origin"] = true;

    for (;;) {
        String h;
        uint32_t gen;
        xSemaphoreTake(lock, portMAX_DELAY);
        h = host;
        gen = host_gen;
        xSemaphoreGive(lock);

        if (gen != seen_gen) {
            meta = FileMeta();
            seen_gen = gen;
            identity_ok = false;
            fails = 0;
            pin_host(h); // right away: the first poll may already need it
        }

        if (h.length() == 0 || WiFi.status() != WL_CONNECTED) {
            xSemaphoreTake(lock, portMAX_DELAY);
            shared.reachable = false;
            xSemaphoreGive(lock);
            // Never let a command fire late, once the link comes back.
            PrinterCommand dropped;
            if (xQueueReceive(cmd_queue, &dropped, pdMS_TO_TICKS(500)) == pdTRUE)
                set_result("Not connected");
            continue;
        }

        xSemaphoreTake(lock, portMAX_DELAY);
        bool forced = force_search;
        force_search = false;
        xSemaphoreGive(lock);
        if (forced) {
            searched = true;
            last_search_ms = millis();
            if (search_printer(h, true)) continue;
        }

        String base = base_url(h);

        PrinterCommand cmd;
        while (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE) run_command(base, cmd);

        JsonDocument doc;
        int http_code = -1;
        bool ok = http_get_json(base +
                                    "/printer/objects/query?webhooks&print_stats&display_status"
                                    "&virtual_sdcard&extruder&heater_bed&toolhead&fan&gcode_move",
                                doc, filter, &http_code);

        PrinterStatus s;
        if (ok) {
            JsonObject r = doc["result"]["status"];
            JsonObject ps = r["print_stats"];

            s.reachable = true;
            s.last_ok_ms = millis();
            copy_str(s.klippy_state, sizeof(s.klippy_state), r["webhooks"]["state"] | "");
            copy_str(s.state, sizeof(s.state), ps["state"] | "");
            const char *msg = ps["message"] | "";
            if (!*msg && strcmp(s.klippy_state, "ready") != 0) msg = r["webhooks"]["state_message"] | "";
            copy_str(s.message, sizeof(s.message), msg);
            copy_str(s.filename, sizeof(s.filename), ps["filename"] | "");

            float p = r["display_status"]["progress"] | 0.0f;
            if (p <= 0) p = r["virtual_sdcard"]["progress"] | 0.0f;
            s.progress = p;

            s.extruder_temp = r["extruder"]["temperature"] | 0.0f;
            s.extruder_target = r["extruder"]["target"] | 0.0f;
            s.extruder_power = r["extruder"]["power"] | 0.0f;
            s.bed_temp = r["heater_bed"]["temperature"] | 0.0f;
            s.bed_target = r["heater_bed"]["target"] | 0.0f;
            s.bed_power = r["heater_bed"]["power"] | 0.0f;
            s.print_duration = ps["print_duration"] | 0.0f;
            s.total_duration = ps["total_duration"] | 0.0f;
            s.filament_used_mm = ps["filament_used"] | 0.0f;
            s.current_layer = ps["info"]["current_layer"] | -1;
            s.fan_speed = r["fan"]["speed"] | 0.0f;
            s.speed_factor = r["gcode_move"]["speed_factor"] | 1.0f;
            s.extrude_factor = r["gcode_move"]["extrude_factor"] | 1.0f;
            s.z_offset = r["gcode_move"]["homing_origin"][2] | 0.0f;
            for (int i = 0; i < 3; i++) s.pos[i] = r["toolhead"]["position"][i] | 0.0f;
            copy_str(s.homed_axes, sizeof(s.homed_axes), r["toolhead"]["homed_axes"] | "");
            s.total_layer = ps["info"]["total_layer"] | -1;

            if (meta.filename != s.filename) fetch_metadata(base, s.filename, meta);
            s.slicer_estimate_s = meta.estimated_time;
            s.filament_total_mm = meta.filament_total;

            // Most slicers don't emit SET_PRINT_STATS_INFO, so derive the
            // layer from Z + the file's layer geometry, like Mainsail does.
            if (s.total_layer < 0) s.total_layer = meta.layer_count;
            if (s.current_layer < 0 && meta.layer_height > 0 && s.print_duration > 0) {
                float z = r["toolhead"]["position"][2] | 0.0f;
                int l = (int)ceilf((z - meta.first_layer_height) / meta.layer_height + 1 - 0.001f);
                if (l < 1) l = 1;
                if (s.total_layer > 0 && l > s.total_layer) l = s.total_layer;
                s.current_layer = l;
            }
        }

        xSemaphoreTake(lock, portMAX_DELAY);
        if (ok) {
            shared = s;
        } else {
            shared.reachable = false;
        }
        shared.last_http_code = http_code;
        xSemaphoreGive(lock);

        // Same LAN, but DHCP may have moved the printer (and handed its old IP
        // to another device): find it again by MAC.
        bool lost = false;
        if (ok) {
            fails = 0;
            if (!identity_ok) {
                identity_ok = check_identity(h);
                lost = !identity_ok;
            }
        } else {
            lost = ++fails >= FAILS_BEFORE_SEARCH;
        }
        if (lost && (!searched || millis() - last_search_ms >= SEARCH_INTERVAL_MS)) {
            searched = true;
            last_search_ms = millis();
            if (search_printer(h, false)) continue;
        }

        // Sleep until the next poll, but wake immediately for a command.
        if (xQueuePeek(cmd_queue, &cmd, pdMS_TO_TICKS(POLL_INTERVAL_MS)) == pdTRUE) continue;
    }
}

} // namespace

void moonraker_start(const String &h, const char *default_mac) {
    lock = xSemaphoreCreateMutex();
    cmd_queue = xQueueCreate(4, sizeof(PrinterCommand));
    host = h;
    Preferences p;
    p.begin("printmon", true);
    mac_saved = p.isKey("mac");
    String m = mac_saved ? p.getString("mac") : String(default_mac ? default_mac : "");
    p.end();
    have_mac = lan_parse_mac(m, printer_mac);
    // Core 0, away from the Arduino loop (LVGL) on core 1.
    xTaskCreatePinnedToCore(poll_task, "moonraker", 8192, nullptr, 1, nullptr, 0); // ~4 KB used
    // Higher priority than polling and the camera.
    xTaskCreatePinnedToCore(estop_task, "estop", 4096, nullptr, 3, &estop_task_handle, 0); // <1 KB used
}

void moonraker_set_host(const String &h) {
    xSemaphoreTake(lock, portMAX_DELAY);
    host = h;
    host_gen++;
    relearn_mac = true;
    shared = PrinterStatus();
    xSemaphoreGive(lock);
}

void moonraker_select_printer(const String &h, const uint8_t mac[6]) {
    xSemaphoreTake(lock, portMAX_DELAY);
    host = h;
    host_gen++;
    memcpy(printer_mac, mac, 6);
    have_mac = true;
    relearn_mac = false;
    mac_saved = true;
    shared = PrinterStatus();
    xSemaphoreGive(lock);
    save_mac(mac);
}

void moonraker_set_printer_mac(const uint8_t mac[6]) {
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(printer_mac, mac, 6);
    have_mac = true;
    relearn_mac = false;
    force_search = true;
    mac_saved = true;
    host_gen++; // re-check identity of the current host
    xSemaphoreGive(lock);
    save_mac(mac);
}

String moonraker_host() {
    if (!lock) return "";
    xSemaphoreTake(lock, portMAX_DELAY);
    String h = host;
    xSemaphoreGive(lock);
    return h;
}

String moonraker_printer_mac() {
    if (!lock) return "";
    xSemaphoreTake(lock, portMAX_DELAY);
    String m = have_mac ? lan_mac_to_string(printer_mac) : "";
    xSemaphoreGive(lock);
    return m;
}

void moonraker_get(PrinterStatus &out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(lock);
}

void moonraker_send(PrinterCommand cmd) {
    if (!cmd_queue) return;
    if (cmd == PrinterCommand::EmergencyStop) {
        // Drop anything still queued (a resume must not fire after an e-stop).
        xQueueReset(cmd_queue);
        set_result("EMERGENCY STOP sending...");
        xTaskNotifyGive(estop_task_handle);
    } else if (xQueueSend(cmd_queue, &cmd, 0) != pdTRUE) {
        set_result("Busy, try again");
    }
}

void moonraker_set_result(const char *text) {
    if (lock) set_result(text);
}

uint32_t moonraker_last_result(char *buf, size_t n) {
    if (!lock) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(buf, cmd_result, n);
    uint32_t t = cmd_result_ms;
    xSemaphoreGive(lock);
    return t;
}
