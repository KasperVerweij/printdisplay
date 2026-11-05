#pragma once

#include <Arduino.h>

// Snapshot of the printer as last seen via Moonraker's HTTP API. Filled in
// by a background FreeRTOS task (so a slow/unreachable printer never stalls
// the LVGL loop); the UI copies it out with moonraker_get().
struct PrinterStatus {
    bool reachable = false;       // last HTTP poll succeeded
    bool searching = false;       // sweeping the LAN for the printer's MAC
    uint32_t last_ok_ms = 0;      // millis() of last successful poll
    int last_http_code = 0;       // of the last poll (<0: connection error)
    char klippy_state[16] = "";   // webhooks.state: ready/startup/shutdown/error
    char state[16] = "";          // print_stats.state: standby/printing/paused/complete/cancelled/error
    char message[96] = "";        // print_stats.message or webhooks.state_message
    char filename[96] = "";
    float progress = 0;           // 0..1
    float extruder_temp = 0, extruder_target = 0, extruder_power = 0;
    float bed_temp = 0, bed_target = 0, bed_power = 0;
    float print_duration = 0;     // seconds actually printing
    float total_duration = 0;     // seconds since start incl. pauses
    float filament_used_mm = 0;
    int current_layer = -1, total_layer = -1;
    float slicer_estimate_s = 0;  // from file metadata, 0 if unknown
    float fan_speed = 0;          // part cooling fan, 0..1
    float speed_factor = 1;       // M220, 1 = 100 %
    float extrude_factor = 1;     // M221
    float z_offset = 0;           // gcode_move.homing_origin[2] (babystepping)
    float pos[3] = {0, 0, 0};     // toolhead position
    char homed_axes[4] = "";      // e.g. "xyz"
    float filament_total_mm = 0;  // from file metadata, 0 if unknown
};

// Starts the polling task. Host is "ip" or "ip:port" (default port 7125).
// default_mac ("aa:bb:..", may be empty) is used until a MAC is saved.
void moonraker_start(const String &host, const char *default_mac);
// Set by the user: the printer's MAC is re-learned from whatever answers there.
void moonraker_set_host(const String &host);
// Picked from the printer list: host and MAC are both known.
void moonraker_select_printer(const String &host, const uint8_t mac[6]);
// The printer's MAC. If it isn't at the current host, the LAN is searched for it.
void moonraker_set_printer_mac(const uint8_t mac[6]);
// Current host; changes on its own when the printer is found at a new IP.
String moonraker_host();
String moonraker_printer_mac(); // "" if not known yet
// Copies the latest snapshot out (thread-safe).
void moonraker_get(PrinterStatus &out);

enum class PrinterCommand : uint8_t { Pause, Resume, EmergencyStop, FirmwareRestart, KlipperRestart };

// Queues a command for the polling task (non-blocking; result reported via
// moonraker_last_result). Emergency stop is sent right away on its own
// connection (retried until confirmed) and clears any queued commands.
void moonraker_send(PrinterCommand cmd);
// Sets the result text shown on screen (for the API task in printer_api.cpp).
void moonraker_set_result(const char *text);
// Copies the last command's result text; returns its millis() timestamp (0 = none yet).
uint32_t moonraker_last_result(char *buf, size_t n);
