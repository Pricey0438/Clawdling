#include "http_client.h"
#include "certs.h"
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

namespace http_client {

// Push mbedTLS allocations to PSRAM. Internal DRAM has ~38 KB free
// with LVGL + NimBLE + WiFi running — well below the ~40 KB the TLS
// handshake needs. PSRAM has ~7.7 MB free, so SPRAM-backed calloc
// is the obvious fix. Set once on first http call so we don't slow
// down setup() for users who never configure WiFi.
static void* psram_calloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
}

static void ensure_mbedtls_psram() {
    static bool set = false;
    if (!set) {
        mbedtls_platform_set_calloc_free(psram_calloc, free);
        set = true;
    }
}

// Reuse one TLS client across calls. Cert is parsed once.
static WiFiClientSecure& tls_client() {
    static WiFiClientSecure c;
    static bool inited = false;
    if (!inited) {
        c.setCACert(ISRG_ROOT_X1);
        inited = true;
    }
    return c;
}

// ── Hostname → IP pin table ─────────────────────────────────────────
// Some carrier hotspots hijack DNS and return NXDOMAIN for Tailscale
// ts.net subdomains (the Funnel URL we poll). Pin a few known Funnel
// ingress IPs so we can connect even when DNS fails. TLS still validates
// against the hostname via the SNI argument to connect(); the cert chain
// goes back to ISRG_ROOT_X1 unchanged.
//
// If Tailscale rotates the Funnel anycast IPs, update this table.
struct HostPin {
    const char* host;
    const char* ip;
};
static const HostPin HOST_PINS[] = {
    // Personal Funnel endpoints are intentionally NOT committed to the public
    // repo. Add your own daemon mapping here for your deployment — only needed
    // on DNS-hijacking hotspots; plain DNS resolution works otherwise. Example:
    {"daemon.example.ts.net", "100.64.0.1"},
};
static constexpr size_t HOST_PIN_COUNT = sizeof(HOST_PINS) / sizeof(HOST_PINS[0]);

// Parse "https://host[:port]/" into host + port. Returns true on success.
// Port defaults to 443 for https. Strips any trailing path components.
static bool parse_https_url(const String& url, String& host, uint16_t& port) {
    const char* PFX = "https://";
    if (!url.startsWith(PFX)) return false;
    int start = strlen(PFX);
    int end = url.indexOf('/', start);
    if (end < 0) end = url.length();
    String hp = url.substring(start, end);
    int colon = hp.indexOf(':');
    if (colon >= 0) {
        host = hp.substring(0, colon);
        port = (uint16_t)hp.substring(colon + 1).toInt();
    } else {
        host = hp;
        port = 443;
    }
    return host.length() > 0;
}

// Lookup the pinned IP for a hostname. Returns nullptr if not pinned.
static const char* pinned_ip_for(const String& host) {
    for (size_t i = 0; i < HOST_PIN_COUNT; i++) {
        if (host == HOST_PINS[i].host) return HOST_PINS[i].ip;
    }
    return nullptr;
}

// Connect to the daemon. If the hostname is pinned, connect to the IP
// directly with SNI=hostname so cert validation still passes; otherwise
// fall through to plain hostname connect (DNS-based).
//
// Always client.stop() first — the WiFiClientSecure singleton can be left
// in a half-closed state after a TLS handshake failure (or any failure
// where stop() didn't run on the previous call), and re-using it without
// a fresh stop() makes every subsequent connect fail too. Stopping a
// not-connected client is a no-op, so this is safe to always call.
static bool connect_daemon(WiFiClientSecure& client, const String& host, uint16_t port) {
    client.stop();
    const char* pinned = pinned_ip_for(host);
    if (pinned) {
        IPAddress ip;
        if (!ip.fromString(pinned)) {
            Serial.printf("http: bad pinned IP '%s' for %s\n", pinned, host.c_str());
            return false;
        }
        Serial.printf("http: connect %s via pinned %s\n", host.c_str(), pinned);
        // 6-arg connect: IP + port + host (for SNI/cert) + CA + cli_cert + cli_key
        return client.connect(ip, port, host.c_str(), ISRG_ROOT_X1, nullptr, nullptr) == 1;
    }
    Serial.printf("http: connect %s via DNS\n", host.c_str());
    return client.connect(host.c_str(), port) == 1;
}

// Send a raw HTTP request on an already-open TLS client. Returns the
// response body length on success (parsed from Content-Length or until
// EOF). Status code is written to *out_status. Body is appended to `out`
// (up to out_max-1, NUL-terminated).
static int read_http_response(WiFiClientSecure& client, char* out, size_t out_max, int* out_status) {
    *out_status = -1;
    // Wait for first byte (status line) with a timeout.
    uint32_t deadline = millis() + 8000;
    while (!client.available()) {
        if (!client.connected()) return -1;
        if (millis() > deadline) { Serial.println("http: response timeout"); return -1; }
        delay(10);
    }
    // Read status line.
    String status_line = client.readStringUntil('\n');
    if (status_line.length() < 12) return -1;
    int sp1 = status_line.indexOf(' ');
    int sp2 = status_line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) return -1;
    *out_status = status_line.substring(sp1 + 1, sp2).toInt();
    // Skip headers — read until blank line.
    int content_length = -1;
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        h.trim();
        if (h.length() == 0) break;
        if (h.startsWith("Content-Length:")) {
            content_length = h.substring(15).toInt();
        }
    }
    // Read body.
    size_t total = 0;
    deadline = millis() + 8000;
    while (total + 1 < out_max) {
        if (client.available()) {
            int c = client.read();
            if (c < 0) break;
            out[total++] = (char)c;
            deadline = millis() + 2000;   // keep-alive while bytes flow
            if (content_length > 0 && (int)total >= content_length) break;
        } else if (!client.connected()) {
            break;
        } else if (millis() > deadline) {
            break;
        } else {
            delay(2);
        }
    }
    out[total] = '\0';
    return (int)total;
}

bool get_state(const String& url, const String& token, char* out, size_t out_max) {
    ensure_mbedtls_psram();
    String host; uint16_t port;
    if (!parse_https_url(url, host, port)) {
        Serial.printf("http: bad url '%s'\n", url.c_str());
        return false;
    }
    WiFiClientSecure& client = tls_client();
    if (!connect_daemon(client, host, port)) {
        Serial.printf("http: /state connect failed\n");
        client.stop();
        return false;
    }
    // Send GET /state with auth + close.
    String req = "GET /state HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Authorization: Bearer " + token + "\r\n";
    req += "Connection: close\r\n\r\n";
    client.print(req);
    int status = -1;
    int n = read_http_response(client, out, out_max, &status);
    client.stop();
    if (status == 200 && n > 0) return true;
    Serial.printf("http: /state status=%d body=%d bytes\n", status, n);
    return false;
}

bool post_refresh(const String& url, const String& token) {
    ensure_mbedtls_psram();
    String host; uint16_t port;
    if (!parse_https_url(url, host, port)) return false;
    WiFiClientSecure& client = tls_client();
    if (!connect_daemon(client, host, port)) {
        Serial.printf("http: /refresh connect failed\n");
        client.stop();
        return false;
    }
    String req = "POST /refresh HTTP/1.1\r\n";
    req += "Host: " + host + "\r\n";
    req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Length: 0\r\n";
    req += "Connection: close\r\n\r\n";
    client.print(req);
    char throwaway[256];
    int status = -1;
    read_http_response(client, throwaway, sizeof(throwaway), &status);
    client.stop();
    Serial.printf("http: /refresh status=%d\n", status);
    return status == 200;
}

}  // namespace http_client
