#pragma once

#include <Arduino.h>
#include <functional>

// Finds a device on the local Wi-Fi subnet by its MAC address, using ARP.
// On networks where DHCP hands the printer a different IP (or gives its old
// IP to someone else), the MAC is the one thing that stays the same.
// The lookups block (ARP replies take a moment) and must not run on the
// LVGL loop; the Moonraker polling task and the printer scan use them.

// True if ip is IPv4 and inside this display's Wi-Fi subnet.
bool lan_in_subnet(IPAddress ip);

// ARP sweep of the subnet (capped at 1022 hosts): calls on_host once per
// (IP, MAC) that answers, until it returns true. With an IP conflict, the
// same IP comes up once per device. Stops early if cancelled() says so.
// About 8 s for a /24.
bool lan_sweep(const std::function<bool(IPAddress ip, const uint8_t *mac)> &on_host,
               const std::function<bool()> &cancelled = nullptr);

// Keeps this display's ARP entry for ip pointed at mac, even when another
// device on the network uses the same IP (an IP conflict): our traffic for
// ip then still reaches the printer. mac == nullptr (or a non-LAN ip) stops.
void lan_pin(IPAddress ip, const uint8_t mac[6]);

// Points the ARP entry for ip at mac once (until the next ARP traffic
// changes it). For probing one device behind an IP conflict.
void lan_pin_once(IPAddress ip, const uint8_t mac[6]);

// "aa:bb:cc:dd:ee:ff" (also accepts '-' separators) <-> bytes.
bool lan_parse_mac(const String &s, uint8_t mac[6]);
String lan_mac_to_string(const uint8_t mac[6]);
