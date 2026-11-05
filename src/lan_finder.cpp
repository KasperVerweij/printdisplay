#include "lan_finder.h"

#include <WiFi.h>
#include "fixed_list.h"
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/priv/tcpip_priv.h>
#include <netif/ethernet.h>

namespace {

// lwIP's ARP table only holds ARP_TABLE_SIZE (10) entries, so sweep in
// batches small enough that the replies still fit.
constexpr int BATCH = 8;
constexpr uint32_t BATCH_WAIT_MS = 250;
constexpr uint32_t MAX_HOSTS = 1022; // a /22; larger subnets: sweep the /22 around us
constexpr size_t MAX_REPORTED = 384; // (IP, MAC) pairs per sweep, ~4.6 KB

enum class Op { Request, Lookup, Collect, Pin, Hook };

// ARP functions are not thread-safe: run them on lwIP's tcpip thread.
struct ArpCall {
    tcpip_api_call_data call; // must be first
    Op op;
    uint32_t local;           // our IPv4, network order (finds the STA netif)
    uint32_t ips[BATCH];      // network order
    int n;
    uint8_t mac[6];
    bool found;
    // Collect: every stable entry on our netif (ARP_TABLE_SIZE is 10).
    uint32_t got_ip[ARP_TABLE_SIZE];
    uint8_t got_mac[ARP_TABLE_SIZE][6];
    int got;
};

netif *sta_netif(uint32_t local) {
    netif *n;
    NETIF_FOREACH(n) {
        if (netif_is_up(n) && ip4_addr_get_u32(netif_ip4_addr(n)) == local) return n;
    }
    return nullptr;
}

// ARP spy. With an IP conflict, two devices answer ARP for the same IP and
// lwIP's table keeps whichever reply came last. Wrapping the netif's input
// sees every reply, so a sweep finds both devices.
struct Seen {
    uint32_t ip; // network order
    uint8_t mac[6];
};
constexpr int SPY_RING = 64;
Seen spy_ring[SPY_RING];
volatile int spy_head = 0, spy_tail = 0;
volatile bool spying = false;
portMUX_TYPE spy_mux = portMUX_INITIALIZER_UNLOCKED;
netif_input_fn orig_input = nullptr;

// Runs in the Wi-Fi driver's task for every received frame: keep it short.
err_t spy_input(pbuf *p, netif *inp) {
    if (spying && p->len >= 42) {
        const uint8_t *f = static_cast<const uint8_t *>(p->payload);
        if (f[12] == 0x08 && f[13] == 0x06 && f[20] == 0 && (f[21] == 1 || f[21] == 2)) {
            Seen e;
            memcpy(e.mac, f + 22, 6);
            memcpy(&e.ip, f + 28, 4);
            portENTER_CRITICAL(&spy_mux);
            int next = (spy_head + 1) % SPY_RING;
            if (next != spy_tail) {
                spy_ring[spy_head] = e;
                spy_head = next;
            }
            portEXIT_CRITICAL(&spy_mux);
        }
    }
    return orig_input(p, inp);
}

void install_spy(netif *nif) {
    if (nif->input == spy_input) return;
    orig_input = nif->input;
    nif->input = spy_input;
}

bool spy_take(Seen &e) {
    bool got = false;
    portENTER_CRITICAL(&spy_mux);
    if (spy_tail != spy_head) {
        e = spy_ring[spy_tail];
        spy_tail = (spy_tail + 1) % SPY_RING;
        got = true;
    }
    portEXIT_CRITICAL(&spy_mux);
    return got;
}


err_t arp_call(tcpip_api_call_data *d) {
    ArpCall *c = reinterpret_cast<ArpCall *>(d);
    netif *nif = sta_netif(c->local);
    if (!nif) return ERR_IF;
    switch (c->op) {
    case Op::Request:
        for (int i = 0; i < c->n; i++) {
            ip4_addr_t a;
            ip4_addr_set_u32(&a, c->ips[i]);
            etharp_request(nif, &a);
        }
        break;
    case Op::Lookup: {
        ip4_addr_t a;
        ip4_addr_set_u32(&a, c->ips[0]);
        eth_addr *eth;
        const ip4_addr_t *ip_ret;
        if (etharp_find_addr(nif, &a, &eth, &ip_ret) >= 0) {
            memcpy(c->mac, eth->addr, 6);
            c->found = true;
        }
        break;
    }
    case Op::Collect:
        c->got = 0;
        for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
            ip4_addr_t *ip;
            netif *en;
            eth_addr *eth;
            if (etharp_get_entry(i, &ip, &en, &eth) && en == nif) {
                c->got_ip[c->got] = ip4_addr_get_u32(ip);
                memcpy(c->got_mac[c->got], eth->addr, 6);
                c->got++;
            }
        }
        break;
    case Op::Hook:
        install_spy(nif);
        break;
    case Op::Pin: {
        // A made-up ARP reply "ips[0] is at mac", addressed to us. lwIP
        // treats it like a real one and points its ARP entry at mac.
        constexpr uint16_t LEN = 14 + 28;
        pbuf *p = pbuf_alloc(PBUF_RAW, LEN, PBUF_RAM);
        if (!p) return ERR_MEM;
        uint8_t *f = static_cast<uint8_t *>(p->payload);
        const uint8_t *me = nif->hwaddr;
        uint32_t my_ip = c->local;
        memcpy(f, me, 6);           // eth dst
        memcpy(f + 6, c->mac, 6);   // eth src
        f[12] = 0x08, f[13] = 0x06; // ARP
        const uint8_t arp_hdr[8] = {0, 1, 8, 0, 6, 4, 0, 2}; // Ethernet, IPv4, reply
        memcpy(f + 14, arp_hdr, 8);
        memcpy(f + 22, c->mac, 6);  // sender MAC
        memcpy(f + 28, &c->ips[0], 4);
        memcpy(f + 32, me, 6);      // target: us
        memcpy(f + 38, &my_ip, 4);
        ethernet_input(p, nif); // takes the pbuf
        break;
    }
    }
    return ERR_OK;
}

// Pinned printer address (see lan_pin), re-asserted by pin_task.
portMUX_TYPE pin_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t pin_ip = 0;
uint8_t pin_mac[6];
TaskHandle_t pin_task_handle = nullptr;
constexpr uint32_t PIN_INTERVAL_MS = 200;

bool run(ArpCall &c) {
    c.local = (uint32_t)WiFi.localIP();
    c.found = false;
    return c.local && tcpip_api_call(arp_call, &c.call) == ERR_OK;
}

} // namespace

// Another device with the same IP answers ARP too, and lwIP also updates
// the entry from any IP packet that device sends. So keep re-pinning: the
// entry only points the wrong way for a moment, and TCP retransmits.
void pin_task(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PIN_INTERVAL_MS));
        ArpCall c = {};
        portENTER_CRITICAL(&pin_mux);
        c.ips[0] = pin_ip;
        memcpy(c.mac, pin_mac, 6);
        portEXIT_CRITICAL(&pin_mux);
        if (!c.ips[0] || WiFi.status() != WL_CONNECTED) continue;
        c.op = Op::Pin;
        run(c);
    }
}

bool lan_in_subnet(IPAddress ip) {
    uint32_t mask = (uint32_t)WiFi.subnetMask();
    uint32_t local = (uint32_t)WiFi.localIP();
    return local && (uint32_t)ip && ((uint32_t)ip & mask) == (local & mask);
}

bool lan_sweep(const std::function<bool(IPAddress, const uint8_t *)> &on_host,
               const std::function<bool()> &cancelled) {
    uint32_t mask = ntohl((uint32_t)WiFi.subnetMask());
    uint32_t local = ntohl((uint32_t)WiFi.localIP());
    if (!local) return false;
    uint32_t host_bits = ~mask;
    if (host_bits > MAX_HOSTS + 1) host_bits = MAX_HOSTS + 1; // /22 window containing us
    uint32_t net = local & ~host_bits;

    ArpCall c = {};
    c.op = Op::Hook;
    run(c);
    Seen e;
    while (spy_take(e)) {
    }
    spying = true;

    // Report each (IP, MAC) pair once. An IP can come up twice: a conflict.
    FixedList<Seen> reported(MAX_REPORTED);
    if (!reported.ok()) {
        Serial.println("[lan] sweep skipped: out of memory");
        spying = false;
        return false;
    }
    bool stop = false;
    auto report = [&](uint32_t ip_n, const uint8_t *mac) {
        uint32_t ip = ntohl(ip_n);
        if ((ip & ~host_bits) != net || ip == local) return;
        for (const Seen &r : reported)
            if (r.ip == ip_n && !memcmp(r.mac, mac, 6)) return;
        Seen r;
        r.ip = ip_n;
        memcpy(r.mac, mac, 6);
        if (!reported.push(r)) return; // full: ignore the rest
        if (on_host(IPAddress(ip_n), mac)) stop = true;
    };
    auto drain = [&]() {
        while (!stop && spy_take(e)) report(e.ip, e.mac);
        // The table too, for devices we heard from before the sweep.
        c.op = Op::Collect;
        if (!stop && run(c))
            for (int i = 0; i < c.got && !stop; i++) report(c.got_ip[i], c.got_mac[i]);
    };

    drain();
    for (uint32_t h = 1; h < host_bits && !stop;) {
        c.n = 0;
        for (; h < host_bits && c.n < BATCH; h++) {
            uint32_t ip = net | h;
            if (ip != local) c.ips[c.n++] = htonl(ip);
        }
        c.op = Op::Request;
        if (!run(c)) break;
        delay(BATCH_WAIT_MS);
        drain();
        if (WiFi.status() != WL_CONNECTED || (cancelled && cancelled())) break;
    }
    if (!stop) {
        delay(BATCH_WAIT_MS); // late replies
        drain();
    }
    spying = false;
    return stop;
}

void lan_pin(IPAddress ip, const uint8_t mac[6]) {
    uint32_t v = mac && lan_in_subnet(ip) ? (uint32_t)ip : 0;
    portENTER_CRITICAL(&pin_mux);
    bool changed = v != pin_ip || (v && memcmp(mac, pin_mac, 6));
    pin_ip = v;
    if (v) memcpy(pin_mac, mac, 6);
    portEXIT_CRITICAL(&pin_mux);
    if (changed && v)
        Serial.printf("[lan] pinning %s to %s\n", ip.toString().c_str(), lan_mac_to_string(mac).c_str());
    if (v && !pin_task_handle) xTaskCreatePinnedToCore(pin_task, "arppin", 3072, nullptr, 2, &pin_task_handle, 0);
    if (v) {
        ArpCall c = {};
        c.op = Op::Pin;
        c.ips[0] = v;
        memcpy(c.mac, mac, 6);
        run(c);
    }
}

void lan_pin_once(IPAddress ip, const uint8_t mac[6]) {
    ArpCall c = {};
    c.op = Op::Pin;
    c.ips[0] = (uint32_t)ip;
    memcpy(c.mac, mac, 6);
    run(c);
}

bool lan_parse_mac(const String &s, uint8_t mac[6]) {
    unsigned v[6];
    char sep[5];
    String t = s;
    t.trim();
    if (t.length() != 17) return false;
    if (sscanf(t.c_str(), "%2x%c%2x%c%2x%c%2x%c%2x%c%2x", &v[0], &sep[0], &v[1], &sep[1], &v[2], &sep[2], &v[3],
               &sep[3], &v[4], &sep[4], &v[5]) != 11)
        return false;
    for (char c : sep)
        if (c != ':' && c != '-') return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

String lan_mac_to_string(const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}
