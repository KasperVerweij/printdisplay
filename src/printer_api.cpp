#include "printer_api.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include "moonraker.h"

namespace {

enum class Kind : uint8_t { Gcode, Post, Fetch };

struct Request {
    Kind kind;
    ApiList list;
    char arg[192];
    char label[32];
};

QueueHandle_t queue = nullptr;
SemaphoreHandle_t lock = nullptr;

struct List {
    char names[API_LIST_MAX][API_NAME_LEN];
    double modified[API_LIST_MAX]; // files: for newest-first
    int n = 0;
    bool loading = false;
    uint32_t gen = 0;
};
// One cache, for whichever list was fetched last (the menu shows one at a
// time). Guarded by lock.
List cache;
ApiList cache_kind = ApiList::Files;

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

String base_url() {
    String h = moonraker_host();
    return h.indexOf(':') >= 0 ? "http://" + h : "http://" + h + ":7125";
}

void result(const char *label, int code, const char *err) {
    char buf[48];
    if (code == 200)
        snprintf(buf, sizeof(buf), "%s: done", label);
    else if (err && *err)
        snprintf(buf, sizeof(buf), "%s", err); // Klipper's own message, e.g. "Must home axis first"
    else
        snprintf(buf, sizeof(buf), "%s failed (%d)", label, code);
    moonraker_set_result(buf);
}

void do_post(const String &url, const char *label, uint32_t timeout_ms) {
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(timeout_ms);
    http.useHTTP10(true);
    int code = -1;
    char err[48] = "";
    if (http.begin(url)) {
        code = http.POST("");
        if (code > 0 && code != 200) {
            JsonDocument filter;
            filter["error"]["message"] = true;
            JsonDocument doc;
            if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) ==
                DeserializationError::Ok)
                strlcpy(err, doc["error"]["message"] | "", sizeof(err));
        }
        http.end();
    }
    Serial.printf("[api] %s -> %d %s\n", label, code, err);
    result(label, code, err);
}

// Streams a JSON array of objects or strings one element at a time, so a
// long file list never has to fit in RAM. key is the member that holds it.
template <typename F> bool stream_array(const String &url, const char *key, JsonDocument &filter, F each) {
    HTTPClient http;
    http.setConnectTimeout(2000);
    http.setTimeout(5000);
    http.useHTTP10(true);
    if (!http.begin(url)) return false;
    bool ok = false;
    if (http.GET() == 200) {
        Stream &s = http.getStream();
        // Stream reads busy-wait up to the timeout: keep it well under the
        // 5 s task watchdog.
        s.setTimeout(2000);
        if (s.find(key) && s.find("[")) {
            ok = true;
            for (;;) {
                JsonDocument doc;
                DeserializationError e = filter.isNull()
                                             ? deserializeJson(doc, s)
                                             : deserializeJson(doc, s, DeserializationOption::Filter(filter));
                if (e) break;
                each(doc);
                if (!s.findUntil(",", "]")) break;
            }
        }
    }
    http.end();
    return ok;
}

void fetch_files(List &tmp) {
    JsonDocument filter;
    filter["path"] = true;
    filter["modified"] = true;
    stream_array(base_url() + "/server/files/list?root=gcodes", "\"result\"", filter, [&](JsonDocument &doc) {
        const char *path = doc["path"] | "";
        double mod = doc["modified"] | 0.0;
        if (!*path || strlen(path) >= API_NAME_LEN) return; // a cut-off path would start the wrong file
        // Keep the newest API_LIST_MAX, sorted newest first.
        int pos = tmp.n;
        while (pos > 0 && tmp.modified[pos - 1] < mod) pos--;
        if (pos >= API_LIST_MAX) return;
        int last = min(tmp.n, API_LIST_MAX - 1);
        for (int i = last; i > pos; i--) {
            memcpy(tmp.names[i], tmp.names[i - 1], API_NAME_LEN);
            tmp.modified[i] = tmp.modified[i - 1];
        }
        strlcpy(tmp.names[pos], path, API_NAME_LEN);
        tmp.modified[pos] = mod;
        if (tmp.n < API_LIST_MAX) tmp.n++;
    });
}

void fetch_macros(List &tmp) {
    JsonDocument none;
    stream_array(base_url() + "/printer/objects/list", "\"objects\"", none, [&](JsonDocument &doc) {
        const char *obj = doc.as<const char *>();
        if (!obj || strncmp(obj, "gcode_macro ", 12)) return;
        const char *name = obj + 12;
        if (*name == '_' || tmp.n >= API_LIST_MAX) return; // _HELPERS are internal
        strlcpy(tmp.names[tmp.n++], name, API_NAME_LEN);
    });
    qsort(tmp.names, tmp.n, API_NAME_LEN, [](const void *a, const void *b) {
        return strcasecmp(static_cast<const char *>(a), static_cast<const char *>(b));
    });
}

// The temporary list is big (~2.5 KB): keep it off the task stack.
List fetch_tmp;

void api_task(void *) {
    Request r;
    for (;;) {
        if (xQueueReceive(queue, &r, portMAX_DELAY) != pdTRUE) continue;
        if (WiFi.status() != WL_CONNECTED || moonraker_host().isEmpty()) {
            if (r.kind != Kind::Fetch) moonraker_set_result("Not connected");
            continue;
        }
        switch (r.kind) {
        case Kind::Gcode:
            // Long timeout: homing, probing or a macro can take minutes.
            do_post(base_url() + "/printer/gcode/script?script=" + url_encode(r.arg), r.label, 180000);
            break;
        case Kind::Post:
            do_post(base_url() + r.arg, r.label, 15000);
            break;
        case Kind::Fetch: {
            fetch_tmp.n = 0;
            if (r.list == ApiList::Files)
                fetch_files(fetch_tmp);
            else
                fetch_macros(fetch_tmp);
            xSemaphoreTake(lock, portMAX_DELAY);
            if (cache_kind == r.list) { // else the user moved on to the other list
                memcpy(cache.names, fetch_tmp.names, sizeof(cache.names));
                cache.n = fetch_tmp.n;
                cache.loading = false;
                cache.gen++;
            }
            xSemaphoreGive(lock);
            break;
        }
        }
    }
}

} // namespace

void api_start();

namespace {

bool enqueue(Kind k, ApiList w, const char *arg, const char *label) {
    api_start();
    Request r;
    r.kind = k;
    r.list = w;
    strlcpy(r.arg, arg, sizeof(r.arg));
    strlcpy(r.label, label, sizeof(r.label));
    if (xQueueSend(queue, &r, 0) != pdTRUE) {
        moonraker_set_result("Busy, try again");
        return false;
    }
    return true;
}

} // namespace

void api_start() {
    if (queue) return;
    lock = xSemaphoreCreateMutex();
    queue = xQueueCreate(6, sizeof(Request));
    xTaskCreatePinnedToCore(api_task, "api", 6144, nullptr, 1, nullptr, 0);
}

bool api_gcode(const String &script, const char *label) {
    if (!enqueue(Kind::Gcode, ApiList::Files, script.c_str(), label)) return false;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s...", label);
    moonraker_set_result(buf);
    return true;
}

bool api_post(const String &path, const char *label) {
    if (!enqueue(Kind::Post, ApiList::Files, path.c_str(), label)) return false;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s...", label);
    moonraker_set_result(buf);
    return true;
}

void api_fetch(ApiList which) {
    api_start();
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cache_kind != which) {
        cache_kind = which;
        cache.n = 0;
        cache.loading = false;
        cache.gen++;
    }
    bool already = cache.loading;
    cache.loading = true;
    xSemaphoreGive(lock);
    if (!already && !enqueue(Kind::Fetch, which, "", "")) {
        xSemaphoreTake(lock, portMAX_DELAY);
        cache.loading = false;
        xSemaphoreGive(lock);
    }
}

int api_list(ApiList which, bool *loading, uint32_t *gen) {
    if (!lock) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool mine = cache_kind == which;
    int n = mine ? cache.n : 0;
    if (loading) *loading = mine && cache.loading;
    if (gen) *gen = cache.gen;
    xSemaphoreGive(lock);
    return n;
}

bool api_list_name(ApiList which, int i, char *buf, size_t n) {
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cache_kind == which && i >= 0 && i < cache.n;
    if (ok) strlcpy(buf, cache.names[i], n);
    xSemaphoreGive(lock);
    return ok;
}
