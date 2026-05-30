#include "wifi_client.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>

namespace wifi_client {

static String s_ssid;
static String s_pass;
static uint32_t s_last_attempt = 0;
static const uint32_t RECONNECT_INTERVAL_MS = 15000;
static bool s_dns_pinned_this_connect = false;
static bool s_enabled = true;

// DNS override: hotspot DNS servers sometimes positively return NXDOMAIN
// for Tailscale ts.net subdomains (the Funnel URL we poll) rather than
// timing out, so the FALLBACK slot is never consulted. Pin slots MAIN
// + BACKUP + FALLBACK all to public resolvers (Cloudflare, Google) so
// the device's daemon polls work regardless of carrier DNS behavior.
// DHCP DNS is intentionally overridden — we only ever resolve one host
// here (the daemon URL), and that host is publicly-resolvable.
static void apply_dns_fallback(void) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;
    auto set_slot = [&](esp_netif_dns_type_t slot, const char* ip) {
        esp_netif_dns_info_t info = {};
        info.ip.u_addr.ip4.addr = ipaddr_addr(ip);
        info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, slot, &info);
    };
    set_slot(ESP_NETIF_DNS_MAIN,     "1.1.1.1");
    set_slot(ESP_NETIF_DNS_BACKUP,   "8.8.8.8");
    set_slot(ESP_NETIF_DNS_FALLBACK, "9.9.9.9");
}

void begin(const String& ssid, const String& pass) {
    s_ssid = ssid;
    s_pass = pass;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    s_last_attempt = millis();
    s_dns_pinned_this_connect = false;
    Serial.printf("wifi: connecting to %s (DNS pinned: 1.1.1.1)\n", ssid.c_str());
}

bool connected(void) {
    return WiFi.status() == WL_CONNECTED;
}

int rssi(void) {
    return connected() ? WiFi.RSSI() : 0;
}

void tick(void) {
    if (!s_enabled) return;
    if (s_ssid.length() == 0) return;
    if (connected()) {
        // DHCP populates DNS at the moment of connection. Re-pin AFTER
        // we see the link is up so our override takes effect, not just at
        // begin() (where the netif may not even exist yet).
        if (!s_dns_pinned_this_connect) {
            apply_dns_fallback();
            s_dns_pinned_this_connect = true;
            Serial.println("wifi: DNS pinned (1.1.1.1 / 8.8.8.8 / 9.9.9.9)");
        }
        return;
    }
    // Disconnected — re-pin on next connect
    s_dns_pinned_this_connect = false;
    uint32_t now = millis();
    if (now - s_last_attempt < RECONNECT_INTERVAL_MS) return;
    s_last_attempt = now;
    Serial.println("wifi: reconnecting...");
    WiFi.disconnect();
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
}

void set_enabled(bool v) {
    if (v == s_enabled) return;
    s_enabled = v;
    if (!v) {
        Serial.println("wifi: radio OFF (user toggle)");
        WiFi.disconnect(true);     // true = also turn off STA mode
        WiFi.mode(WIFI_OFF);
        s_dns_pinned_this_connect = false;
        return;
    }
    Serial.println("wifi: radio ON (user toggle)");
    if (s_ssid.length() == 0) return;  // nothing to connect to yet
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    s_last_attempt = millis();
}

bool is_enabled(void) { return s_enabled; }

}  // namespace wifi_client
