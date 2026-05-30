#include "config_store.h"
#include <Preferences.h>
#include <esp_attr.h>   // RTC_NOINIT_ATTR
#include <cstring>

namespace cfg {

static Preferences prefs;
static const char* NS = "clawdmeter";

// Cached on init() and updated by save_all()/wipe(). Avoids 4× NVS reads
// on every main-loop tick (callers in ui.cpp + main.cpp hit this at 50–100 Hz).
static bool wifi_cached = false;

// ---- Crash-proof config backup (RTC slow memory) ----
// NVS lives on SPI flash. A crash-reboot can interrupt an NVS write (pet state,
// achievements, etc. write periodically) and corrupt the partition; the next
// nvs_flash_init() then ERASES the whole partition, taking the WiFi/daemon
// creds with it → device strands on the provisioning QR. RTC_NOINIT memory
// survives every software/watchdog/panic reset (only a true power-off clears
// it), so we mirror the config here and restore it on boot when NVS has lost
// it. This makes a crash-reboot unable to wipe the user's setup.
struct RtcCfg {
    uint32_t magic;
    char ssid[CFG_MAX_SSID + 1];
    char pass[CFG_MAX_PASS + 1];
    char url[CFG_MAX_URL + 1];
    char token[CFG_MAX_TOKEN + 1];
    uint32_t crc;
};
static RTC_NOINIT_ATTR RtcCfg s_rtc;   // NOT zeroed on reset (garbage on power-on)
static const uint32_t RTC_MAGIC = 0x0C1A0DBEu;

static uint32_t fnv1a(const char* p, uint32_t h) {
    while (*p) { h ^= (uint8_t)*p++; h *= 16777619u; }
    return h;
}
static uint32_t rtc_crc(const RtcCfg& c) {
    uint32_t h = 2166136261u;
    h = fnv1a(c.ssid, h);  h = fnv1a("\x1f", h);
    h = fnv1a(c.pass, h);  h = fnv1a("\x1f", h);
    h = fnv1a(c.url, h);   h = fnv1a("\x1f", h);
    h = fnv1a(c.token, h);
    return h;
}
static bool rtc_valid(void) {
    return s_rtc.magic == RTC_MAGIC && s_rtc.crc == rtc_crc(s_rtc)
        && s_rtc.ssid[0] != '\0';
}
static void rtc_store(const String& ssid, const String& pass,
                      const String& url, const String& token) {
    strncpy(s_rtc.ssid,  ssid.c_str(),  CFG_MAX_SSID);  s_rtc.ssid[CFG_MAX_SSID]   = '\0';
    strncpy(s_rtc.pass,  pass.c_str(),  CFG_MAX_PASS);  s_rtc.pass[CFG_MAX_PASS]   = '\0';
    strncpy(s_rtc.url,   url.c_str(),   CFG_MAX_URL);   s_rtc.url[CFG_MAX_URL]     = '\0';
    strncpy(s_rtc.token, token.c_str(), CFG_MAX_TOKEN); s_rtc.token[CFG_MAX_TOKEN] = '\0';
    s_rtc.magic = RTC_MAGIC;
    s_rtc.crc   = rtc_crc(s_rtc);
}
static void rtc_clear(void) { s_rtc.magic = 0; s_rtc.crc = 0; s_rtc.ssid[0] = '\0'; }

static bool probe_has_wifi(void) {
    return prefs.isKey("ssid") && prefs.isKey("pass")
        && prefs.isKey("url")  && prefs.isKey("token");
}

void init(void) {
    prefs.begin(NS, /*readOnly=*/false);
    wifi_cached = probe_has_wifi();

    if (!wifi_cached && rtc_valid()) {
        // NVS lost the config (a crash-reboot corrupted/erased the partition)
        // but the RTC backup survived. Restore it so the device boots into
        // normal operation instead of stranding on the provisioning QR.
        Serial.println("cfg: NVS config missing but RTC backup valid — restoring");
        prefs.putString("ssid",  s_rtc.ssid);
        prefs.putString("pass",  s_rtc.pass);
        prefs.putString("url",   s_rtc.url);
        prefs.putString("token", s_rtc.token);
        wifi_cached = probe_has_wifi();
    } else if (wifi_cached) {
        // NVS is the source of truth and it's intact — mirror it into RTC so
        // the backup is fresh for the next crash. (Also covers first boot after
        // this firmware update, when the RTC backup doesn't exist yet.)
        String ssid, pass, url, token;
        if (load_wifi(ssid, pass) && load_daemon(url, token)) {
            rtc_store(ssid, pass, url, token);
        }
    }
}

bool has_wifi(void) {
    return wifi_cached;
}

bool load_wifi(String& ssid, String& pass) {
    if (!prefs.isKey("ssid") || !prefs.isKey("pass")) return false;
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    return ssid.length() > 0;
}

bool load_daemon(String& url, String& token) {
    if (!prefs.isKey("url") || !prefs.isKey("token")) return false;
    url   = prefs.getString("url", "");
    token = prefs.getString("token", "");
    return url.length() > 0 && token.length() > 0;
}

bool save_all(const String& ssid, const String& pass,
              const String& url, const String& token) {
    // Length-check every field up-front. Preferences::putString silently
    // truncates on overflow, so a partial write would store a corrupt value
    // and leak it to NVS. Bail before touching prefs if any field is over.
    struct { const char* name; size_t len; size_t max; } checks[] = {
        {"ssid",  ssid.length(),  CFG_MAX_SSID},
        {"pass",  pass.length(),  CFG_MAX_PASS},
        {"url",   url.length(),   CFG_MAX_URL},
        {"token", token.length(), CFG_MAX_TOKEN},
    };
    for (auto& c : checks) {
        if (c.len > c.max) {
            Serial.printf("cfg: %s too long (got %u, max %u)\n",
                          c.name, (unsigned)c.len, (unsigned)c.max);
            return false;
        }
    }
    bool ok = true;
    ok &= prefs.putString("ssid", ssid)  > 0;
    ok &= prefs.putString("pass", pass)  > 0;
    ok &= prefs.putString("url",  url)   > 0;
    ok &= prefs.putString("token", token) > 0;
    wifi_cached = probe_has_wifi();
    // Mirror into the crash-proof RTC backup so a later NVS wipe can't lose it.
    if (wifi_cached) rtc_store(ssid, pass, url, token);
    return ok;
}

void wipe(void) {
    prefs.clear();
    wifi_cached = false;
    rtc_clear();   // factory reset must clear the backup, else init() restores it
}

void clear_wifi_only(void) {
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.remove("url");
    prefs.remove("token");
    wifi_cached = false;
    rtc_clear();   // "switch network" must clear the backup, else init() restores it
}

}  // namespace cfg
