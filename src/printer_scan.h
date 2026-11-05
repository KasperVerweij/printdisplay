#pragma once

#include <Arduino.h>
#include <functional>

// Finds Moonraker instances on the display's Wi-Fi subnet: an ARP sweep
// finds which devices are up, each one is probed on port 7125, and
// Moonraker itself says its hostname and MAC addresses. The MAC comes from
// the printer, not from ARP, because on a network with IP conflicts ARP
// may answer for a different device with the same IP.

struct FoundPrinter {
    IPAddress ip;
    uint8_t mac[6]; // of the interface that has ip
    char name[32];  // Moonraker's hostname, or "" if it didn't say
    bool seen;      // found by the latest scan (false: only remembered)
};

constexpr int PRINTER_SCAN_MAX = 12;

// The list is kept in NVS, so it's there right away after a reboot. A scan
// updates entries in place (matched by MAC) and appends new ones, so an
// index stays valid.
void printer_scan_start();   // background scan; no-op while one is running
bool printer_scan_running();
int printer_scan_results(FoundPrinter *out, int max);
// Short progress text, e.g. "Finding devices..." or "Checking 12/40".
void printer_scan_progress(char *buf, size_t n);

// What a Moonraker instance says about its own network interfaces.
struct PrinterId {
    uint8_t macs[4][6];
    int n = 0;
    int at_ip = -1; // index of the interface that has the queried IP
    bool has(const uint8_t mac[6]) const;
};
// From /machine/system_info. base is "http://ip:port". Blocks up to ~3 s.
bool printer_identity(const String &base, IPAddress ip, PrinterId &out);

// Blocking scan for the printer with this MAC (on any of its interfaces).
// Returns the IP it's at. Takes ~15 s for a /24; stops early if
// cancelled() says so.
bool printer_find(const uint8_t mac[6], IPAddress &ip, const std::function<bool()> &cancelled = nullptr);
