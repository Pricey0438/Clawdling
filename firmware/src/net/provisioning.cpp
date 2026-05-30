#include "provisioning.h"
#include "config_store.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

namespace provisioning {

static bool s_active = false;
static String s_ap_ssid;
static DNSServer s_dns;
static WebServer s_http(80);
static bool s_save_pending = false;
static uint32_t s_save_at_ms = 0;
static String s_pending_ssid, s_pending_pass, s_pending_url, s_pending_token;

static const char* FORM_HTML =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Clawdmeter setup</title>"
"<style>body{font-family:system-ui;max-width:420px;margin:2em auto;padding:0 1em}"
"input,button{font-size:1em;padding:.5em;width:100%;margin:.25em 0;box-sizing:border-box}"
"label{display:block;margin-top:1em;font-weight:600}</style>"
"<h1>Clawdmeter</h1>"
"<form method=POST action=/save>"
"<label>WiFi SSID<input name=ssid required></label>"
"<label>WiFi password<input name=pass type=password></label>"
"<label>Daemon URL<input name=url required placeholder='https://my-host.ts.net'></label>"
"<label>Bearer token<input name=token required></label>"
"<button type=submit>Save and reboot</button></form>";

static void handle_root() {
    s_http.send(200, "text/html", FORM_HTML);
}

static String form_field(const String& name) {
    if (!s_http.hasArg(name)) return "";
    return s_http.arg(name);
}

static void handle_save() {
    s_pending_ssid  = form_field("ssid");
    s_pending_pass  = form_field("pass");
    s_pending_url   = form_field("url");
    s_pending_token = form_field("token");
    // Strip trailing slash from URL so http_client doesn't produce //state
    if (s_pending_url.endsWith("/")) s_pending_url.remove(s_pending_url.length() - 1);

    if (s_pending_ssid.length() == 0 || s_pending_url.length() == 0 || s_pending_token.length() == 0) {
        s_http.send(400, "text/plain", "ssid, url and token are required");
        return;
    }
    // Reject overlong fields up-front so we don't silently truncate in NVS.
    // Mirrors the cfg::save_all() check; surfacing it here lets the user see
    // the failure in the browser instead of a later HTTPS 401.
    struct { const char* name; size_t len; size_t max; } checks[] = {
        {"ssid",  s_pending_ssid.length(),  cfg::CFG_MAX_SSID},
        {"pass",  s_pending_pass.length(),  cfg::CFG_MAX_PASS},
        {"url",   s_pending_url.length(),   cfg::CFG_MAX_URL},
        {"token", s_pending_token.length(), cfg::CFG_MAX_TOKEN},
    };
    for (auto& c : checks) {
        if (c.len > c.max) {
            char msg[80];
            snprintf(msg, sizeof(msg), "%s too long (got %u, max %u)",
                     c.name, (unsigned)c.len, (unsigned)c.max);
            s_http.send(413, "text/plain", msg);
            return;
        }
    }
    s_http.send(200, "text/html",
        "<meta http-equiv=refresh content=2>"
        "<h1>Saved. Rebooting\xe2\x80\xa6</h1>");
    s_save_pending = true;
    s_save_at_ms = millis() + 1000;  // give browser time to render the page
}

static void handle_404() {
    // Captive-portal: redirect everything to the form
    s_http.sendHeader("Location", "http://192.168.4.1/", true);
    s_http.send(302, "text/plain", "");
}

void begin(void) {
    uint64_t mac = ESP.getEfuseMac();
    char ssidbuf[24];
    snprintf(ssidbuf, sizeof(ssidbuf), "Clawdmeter-%04X", (unsigned)(mac & 0xFFFF));
    s_ap_ssid = ssidbuf;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(s_ap_ssid.c_str());     // open network
    delay(100);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("provisioning: AP=%s ip=%s\n", s_ap_ssid.c_str(), ip.toString().c_str());

    s_dns.start(53, "*", ip);
    s_http.on("/", handle_root);
    s_http.on("/save", HTTP_POST, handle_save);
    s_http.onNotFound(handle_404);
    s_http.begin();
    s_active = true;
}

bool active(void) { return s_active; }

const String& ap_ssid(void) { return s_ap_ssid; }

void tick(void) {
    if (!s_active) return;
    s_dns.processNextRequest();
    s_http.handleClient();
    if (s_save_pending && (int32_t)(millis() - s_save_at_ms) >= 0) {
        cfg::save_all(s_pending_ssid, s_pending_pass, s_pending_url, s_pending_token);
        Serial.println("provisioning: saved, restarting");
        delay(200);
        ESP.restart();
    }
}

}  // namespace provisioning
