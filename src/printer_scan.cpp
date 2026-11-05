#include "printer_scan.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <functional>
#include "fixed_list.h"

#include "lan_finder.h"

namespace {

constexpr uint16_t MOONRAKER_PORT = 7125;
constexpr size_t MAX_HOSTS = 384;         // devices per scan (incl. conflicts), ~6 KB
constexpr int PARALLEL = 4;               // sockets at once (lwIP allows 16 in total)
constexpr uint32_t CONNECT_WAIT_MS = 700; // RST/SYN-ACK is fast; a Pi on Wi-Fi power save can take a few hundred ms

SemaphoreHandle_t lock = nullptr;
SemaphoreHandle_t scan_mutex = nullptr; // one sweep at a time: they'd fight over the ARP table
volatile bool running = false;
FoundPrinter results[PRINTER_SCAN_MAX]; // guarded by lock
int result_count = 0;
bool loaded = false;
int checked = 0, to_check = 0; // 0/0 while still sweeping

// NVS copy of the list. IPAddress isn't a plain struct, so store a POD.
struct Stored {
    uint32_t ip;
    uint8_t mac[6];
    char name[32];
};

void ensure_loaded() {
    if (!lock) lock = xSemaphoreCreateMutex();
    if (!scan_mutex) scan_mutex = xSemaphoreCreateMutex();
    if (loaded) return;
    loaded = true;
    Stored st[PRINTER_SCAN_MAX];
    Preferences p;
    p.begin("printmon", true);
    size_t len = p.isKey("printers") ? p.getBytes("printers", st, sizeof(st)) : 0;
    p.end();
    xSemaphoreTake(lock, portMAX_DELAY);
    result_count = len / sizeof(Stored);
    for (int i = 0; i < result_count; i++) {
        results[i].ip = IPAddress(st[i].ip);
        memcpy(results[i].mac, st[i].mac, 6);
        strlcpy(results[i].name, st[i].name, sizeof(results[i].name));
        results[i].seen = false;
    }
    xSemaphoreGive(lock);
}

void save_list() {
    Stored st[PRINTER_SCAN_MAX];
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = result_count;
    for (int i = 0; i < n; i++) {
        st[i].ip = (uint32_t)results[i].ip;
        memcpy(st[i].mac, results[i].mac, 6);
        strlcpy(st[i].name, results[i].name, sizeof(st[i].name));
    }
    xSemaphoreGive(lock);
    Preferences p;
    p.begin("printmon", false);
    p.putBytes("printers", st, n * sizeof(Stored));
    p.end();
}

// Updates the entry with the same MAC, or appends.
void remember(const FoundPrinter &p) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int i = 0;
    while (i < result_count && memcmp(results[i].mac, p.mac, 6)) i++;
    if (i < result_count) {
        // A lookup that failed this time (IP conflict) keeps the known name.
        char name[sizeof(p.name)];
        strlcpy(name, results[i].name, sizeof(name));
        results[i] = p;
        if (!p.name[0]) strlcpy(results[i].name, name, sizeof(name));
    } else if (result_count < PRINTER_SCAN_MAX)
        results[result_count++] = p;
    xSemaphoreGive(lock);
}

struct Host {
    IPAddress ip;
    uint8_t mac[6]; // the device that answered ARP; there may be two per IP
};

// Non-blocking connects to a batch; open[i] = port 7125 accepted.
void probe_batch(const Host *hosts, int n, bool *open) {
    int fds[PARALLEL];
    for (int i = 0; i < n; i++) {
        open[i] = false;
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] < 0) continue;
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        // Behind an IP conflict, make sure the SYN goes to this device.
        lan_pin_once(hosts[i].ip, hosts[i].mac);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(MOONRAKER_PORT);
        a.sin_addr.s_addr = (uint32_t)hosts[i].ip;
        connect(fds[i], (sockaddr *)&a, sizeof(a)); // EINPROGRESS
    }
    uint32_t start = millis();
    int pending = n;
    bool done[PARALLEL] = {};
    while (pending > 0 && millis() - start < CONNECT_WAIT_MS) {
        fd_set wr;
        FD_ZERO(&wr);
        int maxfd = -1;
        for (int i = 0; i < n; i++) {
            if (fds[i] < 0 || done[i]) continue;
            FD_SET(fds[i], &wr);
            maxfd = max(maxfd, fds[i]);
        }
        if (maxfd < 0) break;
        timeval tv = {0, 50 * 1000};
        if (select(maxfd + 1, nullptr, &wr, nullptr, &tv) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (fds[i] < 0 || done[i] || !FD_ISSET(fds[i], &wr)) continue;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
            open[i] = err == 0;
            done[i] = true;
            pending--;
        }
    }
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0) close(fds[i]);
}

String base_for(IPAddress ip) {
    return "http://" + ip.toString() + ":" + MOONRAKER_PORT;
}

// Moonraker's /printer/info has the machine's hostname.
void fetch_name(IPAddress ip, char *name, size_t n) {
    name[0] = 0;
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    http.useHTTP10(true);
    if (!http.begin(base_for(ip) + "/printer/info")) return;
    if (http.GET() == 200) {
        JsonDocument filter;
        filter["result"]["hostname"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) ==
            DeserializationError::Ok)
            strlcpy(name, doc["result"]["hostname"] | "", n);
    }
    http.end();
}

// Sweeps the subnet and calls on_found for each Moonraker, until it returns
// true. progress (optional) gets (checked, total) after each probe batch.
bool scan_subnet(const std::function<bool(const FoundPrinter &, const PrinterId &)> &on_found,
                 const std::function<void(int, int)> &progress, const std::function<bool()> &cancelled) {
    xSemaphoreTake(scan_mutex, portMAX_DELAY);
    FixedList<Host> hosts(MAX_HOSTS);
    FixedList<bool> done(MAX_HOSTS);
    if (!hosts.ok() || !done.ok()) {
        Serial.println("[scan] skipped: out of memory");
        xSemaphoreGive(scan_mutex);
        return false;
    }
    lan_sweep([&](IPAddress ip, const uint8_t *mac) {
        Host h;
        h.ip = ip;
        memcpy(h.mac, mac, 6);
        hosts.push(h);
        return false;
    }, cancelled);
    Serial.printf("[scan] %u devices on the subnet\n", (unsigned)hosts.size());
    if (progress) progress(0, hosts.size());

    bool stop = false;
    for (size_t i = 0; i < hosts.size(); i++) done.push(false);
    size_t checked = 0;
    while (checked < hosts.size() && !stop) {
        // A batch never holds the same IP twice: each needs its own ARP pin.
        Host batch[PARALLEL];
        size_t idx[PARALLEL];
        int n = 0;
        for (size_t i = 0; i < hosts.size() && n < PARALLEL; i++) {
            if (done[i]) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= batch[k].ip == hosts[i].ip;
            if (dup) continue;
            batch[n] = hosts[i];
            idx[n++] = i;
        }
        bool open[PARALLEL];
        probe_batch(batch, n, open);
        for (int j = 0; j < n && !stop; j++) {
            done[idx[j]] = true;
            if (!open[j]) continue;
            FoundPrinter p = {};
            p.ip = batch[j].ip;
            p.seen = true;
            PrinterId id;
            lan_pin_once(batch[j].ip, batch[j].mac);
            if (!printer_identity(base_for(p.ip), p.ip, id)) continue; // not Moonraker
            memcpy(p.mac, id.macs[id.at_ip >= 0 ? id.at_ip : 0], 6);
            lan_pin_once(batch[j].ip, batch[j].mac);
            fetch_name(p.ip, p.name, sizeof(p.name));
            Serial.printf("[scan] Moonraker at %s: %s %s\n", p.ip.toString().c_str(), p.name,
                          lan_mac_to_string(p.mac).c_str());
            remember(p);
            stop = on_found(p, id);
        }
        checked += n;
        if (progress) progress(checked, hosts.size());
        if (WiFi.status() != WL_CONNECTED || (cancelled && cancelled())) break;
    }
    save_list();
    xSemaphoreGive(scan_mutex);
    return stop;
}

void scan_task(void *) {
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < result_count; i++) results[i].seen = false;
    xSemaphoreGive(lock);

    scan_subnet(
        [](const FoundPrinter &, const PrinterId &) { return false; }, // remembered by scan_subnet
        [](int done, int total) {
            xSemaphoreTake(lock, portMAX_DELAY);
            checked = done;
            to_check = total;
            xSemaphoreGive(lock);
        },
        nullptr);
    running = false;
    vTaskDelete(nullptr);
}

} // namespace

bool PrinterId::has(const uint8_t mac[6]) const {
    for (int i = 0; i < n; i++)
        if (!memcmp(macs[i], mac, 6)) return true;
    return false;
}

bool printer_identity(const String &base, IPAddress ip, PrinterId &out) {
    out = PrinterId();
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(2500);
    http.useHTTP10(true);
    if (!http.begin(base + "/machine/system_info")) return false;
    bool ok = false;
    if (http.GET() == 200) {
        JsonDocument filter;
        JsonObject iface = filter["result"]["system_info"]["network"]["*"].to<JsonObject>();
        iface["mac_address"] = true;
        iface["ip_addresses"][0]["address"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) ==
            DeserializationError::Ok) {
            String want = ip.toString();
            for (JsonPair kv : doc["result"]["system_info"]["network"].as<JsonObject>()) {
                if (out.n >= 4) break;
                if (!lan_parse_mac(kv.value()["mac_address"] | "", out.macs[out.n])) continue;
                for (JsonVariant a : kv.value()["ip_addresses"].as<JsonArray>())
                    if (want == (a["address"] | "")) out.at_ip = out.n;
                out.n++;
            }
            ok = out.n > 0;
        }
    }
    http.end();
    return ok;
}

bool printer_find(const uint8_t mac[6], IPAddress &ip, const std::function<bool()> &cancelled) {
    ensure_loaded();
    return scan_subnet(
        [&](const FoundPrinter &p, const PrinterId &id) {
            if (!id.has(mac)) return false; // any of its interfaces
            ip = p.ip;
            return true;
        },
        nullptr, cancelled);
}

void printer_scan_start() {
    ensure_loaded();
    if (running || WiFi.status() != WL_CONNECTED) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    checked = to_check = 0;
    xSemaphoreGive(lock);
    running = true;
    if (xTaskCreatePinnedToCore(scan_task, "pscan", 6144, nullptr, 1, nullptr, 0) /* ~4.4 KB used */ != pdPASS) running = false;
}

bool printer_scan_running() {
    return running;
}

int printer_scan_results(FoundPrinter *out, int max) {
    ensure_loaded();
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = min(result_count, max);
    for (int i = 0; i < n; i++) out[i] = results[i];
    xSemaphoreGive(lock);
    return n;
}

void printer_scan_progress(char *buf, size_t n) {
    ensure_loaded();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!running)
        strlcpy(buf, "", n);
    else if (to_check == 0)
        strlcpy(buf, "Finding devices...", n);
    else
        snprintf(buf, n, "Checking %d/%d", checked, to_check);
    xSemaphoreGive(lock);
}
