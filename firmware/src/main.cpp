#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <esp_netif.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "session.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "pet.h"
#include "pet_screen.h"
#include "pet_stats_screen.h"
#include "pet_speech.h"
#include "achievements.h"
#include "streak.h"
#include "recap.h"
#include "retirement_screen.h"

#include "hal/board_caps.h"
#include "version.h"
#include "board_self_id.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/i2c_health.h"

#include "net/config_store.h"
#include "settings.h"
#include "net/wifi_client.h"
#include "net/http_client.h"
#include "net/net_daemon.h"
#include "net/provisioning.h"
#include "radio.h"

static UsageData usage = {};
static SessionList sessions = {};

// OOBE auto-trigger timing. s_boot_ms is set once in setup(); s_oobe_decided
// latches true after the 60s window fires (or sim oobe runs) so the poll
// doesn't repeat. s_oobe_auto_shown is set only by the auto-trigger (not
// by sim) and gates the auto-hide path so a manual "sim oobe" on a device
// that already has wifi/ble-data is not immediately reverted.
static uint32_t s_boot_ms        = 0;
static bool     s_oobe_decided   = false;
static bool     s_oobe_auto_shown = false;

const SessionList* current_sessions(void) { return &sessions; }

static uint32_t g_last_apply_ms = 0;
extern "C" uint32_t app_last_apply_ms(void) { return g_last_apply_ms; }

// ---- LVGL draw buffers (PSRAM, partial render mode) ----
#define BUF_LINES 40
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    // Lock comes first: while locked we don't let touch wake the panel
    // (bag/pocket presses would defeat the 30s fast-sleep) AND we swallow
    // the touch action so widget click handlers stay inert. Only physical
    // buttons can wake the locked screen.
    if (ui_is_locked()) {
        pressed = false;
    } else if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct       = doc["s"]  | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct        = doc["w"]  | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok    = doc["ok"] | false;
    out->valid = true;

    // Wall-clock anchor from daemon. Present only when daemon has a valid
    // system clock; gate on ts > 0 to ignore absent / zero payload field.
    int ts = doc["ts"] | 0;
    if (ts > 0) {
        pet_set_wallclock((uint32_t)ts);
    }
    // TZ offset (signed seconds, e.g. PST=-28800, UTC=0, AEST=+36000).
    // Field absent in legacy daemon JSON — leave g_tz_offset_sec at the NVS
    // value. Real offsets fit in ±50400 (±14h); we treat values outside ±90000
    // as 'unset' to avoid a sentinel ambiguity with valid 0. pet_set_tz_offset
    // is a no-op on identical values so this is safe to call every payload.
    if (doc["tz"].is<int>()) {
        int tz = doc["tz"].as<int>();
        if (tz >= -90000 && tz <= 90000) {
            pet_set_tz_offset((int32_t)tz);
        }
    }

    // Optional: extract session list. If absent, leave existing list intact
    // (firmware keeps last known view across messages).
    JsonArrayConst arr = doc["ses"].as<JsonArrayConst>();
    if (!arr.isNull()) {
        sessions.count = 0;
        unsigned arr_size = (unsigned)arr.size();
        for (JsonObjectConst o : arr) {
            if (sessions.count >= SESSION_MAX) break;
            SessionInfo& s = sessions.items[sessions.count];
            const char* id = o["id"] | "";
            const char* n  = o["n"]  | "";
            strlcpy(s.id,   id, sizeof(s.id));
            strlcpy(s.name, n,  sizeof(s.name));
            // Explicitly type each default so ArduinoJson picks the matching
            // extraction overload — a bare `| 0` is `int` and a JSON value
            // above INT32_MAX truncates negative before reaching the
            // unsigned cast.
            int c = o["c"] | (int)0;
            if (c < 0) c = 0;
            if (c > 100) c = 100;
            s.ctx_pct = (uint8_t)c;
            const char* st = o["st"] | "i";
            switch (st[0]) {
                case 't': s.state = SESSION_THINKING;   break;
                case 'w': s.state = SESSION_WAITING;    break;
                case 'c': s.state = SESSION_COMPACTING; break;
                default:  s.state = SESSION_IDLE;       break;
            }
            uint32_t a = o["a"] | (uint32_t)0;
            // P2-11: surface silent clamp so we notice if a peer ever sends
            // out-of-range ages (>18h). Clamp behaviour unchanged.
            if (a > 65535) {
                Serial.printf("apply: age clamped %u -> 65535\n", (unsigned)a);
            }
            s.age_sec = a > 65535 ? 65535 : (uint16_t)a;
            sessions.count++;
        }
        sessions.valid = true;
        // P2-10: surface silent truncation at SESSION_MAX so we notice if a
        // peer starts pushing more sessions than the firmware can hold.
        if (arr_size > SESSION_MAX) {
            Serial.printf("apply: truncated %u sessions to %u\n",
                          arr_size, (unsigned)SESSION_MAX);
        }
    }
    return true;
}

// ---- Serial command buffer ----
// Sized for `set token=<512-char value>` + key + `\n` + slack. Smaller buffers
// fragment long inputs into multiple parsed commands, corrupting config.
#define CMD_BUF_SIZE 1024
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            String line(cmd_buf);
            if (line == "screenshot") {
                send_screenshot();
            } else if (line == "cfg") {
                String ssid, pass, url, token;
                cfg::load_wifi(ssid, pass);
                cfg::load_daemon(url, token);
                Serial.printf("cfg: ssid=<%s> url=<%s> has_token=%d has_wifi=%d\n",
                              ssid.c_str(), url.c_str(),
                              token.length() > 0 ? 1 : 0,
                              cfg::has_wifi() ? 1 : 0);
            } else if (line == "dns") {
                esp_netif_t* nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (!nf) { Serial.println("dns: no STA netif"); }
                else {
                    const esp_netif_dns_type_t slots[3] = {
                        ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK
                    };
                    const char* names[3] = {"MAIN", "BACKUP", "FALLBACK"};
                    for (int i = 0; i < 3; i++) {
                        esp_netif_dns_info_t info;
                        if (esp_netif_get_dns_info(nf, slots[i], &info) == ESP_OK) {
                            uint32_t a = info.ip.u_addr.ip4.addr;
                            Serial.printf("dns: %s=%u.%u.%u.%u\n", names[i],
                                          (unsigned)(a & 0xff), (unsigned)((a>>8) & 0xff),
                                          (unsigned)((a>>16) & 0xff), (unsigned)((a>>24) & 0xff));
                        }
                    }
                }
            } else if (line == "wifi") {
                Serial.printf("wifi: connected=%d rssi=%d\n",
                              wifi_client::connected() ? 1 : 0,
                              wifi_client::rssi());
            } else if (line.startsWith("set ")) {
                // Dev-only: "set <key>=<value>" — one key per call, value
                // may contain spaces. Reads existing values and overwrites
                // only the named key, so you can run four `set`s in a row
                // without losing previously-set fields.
                String pair = line.substring(4);
                int eq = pair.indexOf('=');
                if (eq < 0) {
                    Serial.println("set: usage: set <ssid|pass|url|token>=<value>");
                } else {
                    String k = pair.substring(0, eq);
                    String v = pair.substring(eq + 1);
                    String ssid, pass, url, token;
                    cfg::load_wifi(ssid, pass);
                    cfg::load_daemon(url, token);
                    bool known = true;
                    size_t max_len = 0;
                    if      (k == "ssid")  { max_len = cfg::CFG_MAX_SSID;  }
                    else if (k == "pass")  { max_len = cfg::CFG_MAX_PASS;  }
                    else if (k == "url")   { max_len = cfg::CFG_MAX_URL;   }
                    else if (k == "token") { max_len = cfg::CFG_MAX_TOKEN; }
                    else { Serial.println("set: unknown key"); known = false; }
                    if (known && v.length() > max_len) {
                        Serial.printf("set: %s too long (got %u, max %u)\n",
                                      k.c_str(), (unsigned)v.length(), (unsigned)max_len);
                        known = false;
                    }
                    if (known) {
                        if      (k == "ssid")  ssid  = v;
                        else if (k == "pass")  pass  = v;
                        else if (k == "url")   url   = v;
                        else if (k == "token") token = v;
                        if (cfg::save_all(ssid, pass, url, token)) {
                            Serial.printf("set: %s saved (restart to apply)\n", k.c_str());
                        } else {
                            Serial.printf("set: %s save failed\n", k.c_str());
                        }
                    }
                }
            } else if (line == "restart") {
                Serial.println("restart: rebooting...");
                delay(100);
                ESP.restart();
            } else if (line == "netd") {
                Serial.printf("netd: fresh=%d latest=%s\n",
                              net_daemon::state_fresh() ? 1 : 0,
                              net_daemon::latest());
            } else if (line == "poll") {
                String url, token;
                if (!cfg::load_daemon(url, token)) {
                    Serial.println("poll: no config");
                } else {
                    static char buf[2048];
                    bool ok = http_client::get_state(url, token, buf, sizeof(buf));
                    Serial.printf("poll: ok=%d body=%s\n", ok ? 1 : 0, ok ? buf : "");
                }
            } else if (line == "wallclock") {
                uint32_t wc  = pet_wallclock_now();
                uint32_t day = pet_today_index();
                Serial.printf("[wallclock] epoch=%u day_index=%u\n", wc, day);
            } else if (line.startsWith("shiny ")) {
                String arg = line.substring(6);
                arg.trim();
                bool on = (arg == "on");
                pet_set_shiny(on);
                Serial.printf("[shiny] %s\n", on ? "ON" : "OFF");
            } else if (line == "vacation on") {
                pet_set_vacation(true);
                Serial.println("[vacation] ON");
            } else if (line == "vacation off") {
                pet_set_vacation(false);
                Serial.println("[vacation] OFF");
            } else if (line.startsWith("sim low-batt")) {
                String arg = line.substring(12);
                arg.trim();
                if (arg == "off") {
                    power_hal_set_sim_battery(-1);
                    Serial.println("sim low-batt: cleared override");
                } else {
                    int pct = arg.length() ? arg.toInt() : 5;
                    power_hal_set_sim_battery(pct);
                    Serial.printf("sim low-batt: forced %d%%\n", pct);
                }
            } else if (line == "sim recap-now") {
                recap_force_reshow();
                ui_show_screen(SCREEN_PET);
                Serial.println("sim recap-now: forced");
            } else if (line == "sim lock") {
                ui_toggle_lock();
                Serial.printf("[sim] lock toggled: now %s\n",
                              ui_is_locked() ? "LOCKED" : "UNLOCKED");
            } else if (line.startsWith("sim tap-stat ")) {
                int idx = line.substring(13).toInt();
                Serial.printf("[sim] tap-stat %d\n", idx);
                pet_stats_screen_sim_tap(idx);
            } else if (line == "sim oobe") {
                Serial.println("[sim] forcing OOBE screen");
                ui_show_screen(SCREEN_OOBE);
                // Latch the 60s decision so the auto-trigger doesn't re-fire.
                // We intentionally do NOT set s_oobe_auto_shown — auto-hide
                // is gated on it, so a sim on a configured device stays
                // visible (otherwise auto-hide would revert immediately).
                s_oobe_decided = true;
            } else if (line.startsWith("sim vacation ")) {
                int days = line.substring(13).toInt();
                if (days <= 0) {
                    Serial.println("sim vacation: usage: sim vacation <days>");
                } else {
                    int clamped = days;
                    if (clamped > (int)VACATION_MAX_DAYS) clamped = (int)VACATION_MAX_DAYS;
                    pet_vacation_start((uint32_t)clamped);
                    if (clamped != days) {
                        Serial.printf("sim vacation: %d days set (clamped from %d)\n", clamped, days);
                    } else {
                        Serial.printf("sim vacation: %d days set\n", clamped);
                    }
                }
            } else if (line == "sim hatch") {
                pet_test_clear_and_rehatch();
                Serial.println("sim hatch: pet wiped, hatch flow next boot/screen entry");
            } else if (line == "sim nvs-erase") {
                // Nuclear NVS recovery — erases the entire NVS partition
                // (every namespace) and reboots. Use this when prefs.clear()
                // has left enough fragmented/tombstoned entries that fresh
                // writes fail with NOT_ENOUGH_SPACE. After reboot the device
                // boots into provisioning mode (no WiFi config) — re-enter
                // creds via captive portal. Same effect as BOOT-held-5s.
                Serial.println("[sim] nvs-erase: erasing NVS partition");
                esp_err_t r1 = nvs_flash_erase();
                Serial.printf("[sim] nvs_flash_erase = %d\n", (int)r1);
                Serial.println("[sim] nvs-erase: rebooting");
                delay(200);
                ESP.restart();
            } else if (line == "sim factory-pet") {
                // Wipe all pet-content NVS namespaces and reboot. Boot path
                // reloads everything from cleared NVS = fresh-product state.
                // Does NOT touch the "clawdmeter" namespace — WiFi/daemon
                // creds survive. Use BOOT-held-5s for a true factory reset.
                Serial.println("[sim] factory-pet: wiping pet/ach/strk/recap");
                {
                    Preferences fp;
                    const char* nss[] = {"pet", "ach", "strk", "recap"};
                    for (size_t i = 0; i < sizeof(nss)/sizeof(nss[0]); i++) {
                        if (fp.begin(nss[i], /*readOnly=*/false)) {
                            fp.clear();
                            fp.end();
                            Serial.printf("[sim] factory-pet: cleared NVS '%s'\n", nss[i]);
                        }
                    }
                }
                Serial.println("[sim] factory-pet: rebooting for clean reload");
                delay(200);
                ESP.restart();
            } else if (line == "sim graduate") {
                pet_force_graduate();
                Serial.println("sim graduate: ceremony triggered");
            } else if (line == "speech") {
                speech_category_t cat = pet_speech_current_category();
                speech_category_t pick_cat = (cat == SPEECH_NONE) ? SPEECH_IDLE : cat;
                uint32_t salt = (uint32_t)(millis() / 60000);
                const char* text = pet_speech_pick(pick_cat, salt);
                Serial.printf("[speech] cat=%d text=\"%s\"\n", (int)pick_cat, text);
                pet_screen_force_speech_bubble(text);
            } else if (line == "retirement") {
                ui_show_screen(SCREEN_RETIREMENT);
                Serial.println("[retirement] showing retirement screen");
            } else if (line == "fake_grads") {
                pet_test_populate_graduates(8);
                Serial.println("[fake_grads] populated 8 test graduates");
            } else if (line == "catalog") {
                ui_show_screen(SCREEN_CATALOG);
                Serial.println("[catalog] showing catalog screen");
            } else if (line == "usage") {
                ui_show_screen(SCREEN_USAGE);
            } else if (line == "session") {
                ui_show_screen(SCREEN_SESSION);
            } else if (line == "pet") {
                ui_show_screen(SCREEN_PET);
            } else if (line == "menu") {
                ui_show_screen(SCREEN_MENU);
                Serial.println("[menu] showing menu screen");
            } else if (line == "stats") {
                ui_show_screen(SCREEN_STATS);
                Serial.println("[stats] showing stats screen");
            } else if (line.startsWith("rename ")) {
                String arg = line.substring(7);
                arg.trim();
                pet_rename(arg.c_str());
                Serial.printf("[rename] input=\"%s\" len=%u\n",
                              arg.c_str(), (unsigned)arg.length());
            } else if (line == "petname") {
                char buf[16];
                pet_get_name(buf, sizeof(buf));
                Serial.printf("[petname] stored=\"%s\" bytes=%u\n",
                              buf, (unsigned)strlen(buf));
            } else if (line == "achievements") {
                ui_show_screen(SCREEN_ACHIEVEMENTS);
                Serial.println("[ach] showing achievements screen");
            } else if (line.startsWith("ach_unlock ")) {
                int id = line.substring(11).toInt();
                if (id >= 0 && id < (int)ACHIEVEMENTS_MAX) {
                    achievements_force_unlock((uint8_t)id);
                } else {
                    Serial.printf("[ach] ach_unlock: id %d out of range (0..%d)\n",
                                  id, (int)ACHIEVEMENTS_MAX - 1);
                }
            } else if (line == "streak") {
                Serial.printf("[streak] care=%u/%u use=%u/%u day=%u\n",
                              (unsigned)streak_care_days(), (unsigned)streak_care_best(),
                              (unsigned)streak_usage_days(), (unsigned)streak_usage_best(),
                              (unsigned)pet_today_index());
            } else if (line.startsWith("streak_force ")) {
                int days = line.substring(13).toInt();
                streak_test_advance_days((uint16_t)days);
                Serial.printf("[streak] advanced %d days\n", days);
            } else if (line.startsWith("streak_set ")) {
                int days = line.substring(11).toInt();
                streak_test_set_days((uint16_t)days);
            } else if (line == "recap") {
                pet_screen_force_recap();
                Serial.println("[recap] showing recap modal");
            } else if (line == "recap_snap") {
                recap_force_snapshot();
                Serial.println("[recap] snapshotted today and reset");
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    s_boot_ms = millis();
    Serial.printf("[version] firmware=%s sha=%s board=%s\n",
                  FW_VERSION_STR, FW_VERSION, FW_BOARD_ENV);
    // Refuse to drive the display if this firmware was flashed to the wrong
    // board. Probes I2C 0x20 (IO expander present only on AMOLED-1.8);
    // mismatch halts with a serial log so future cross-flashes are impossible
    // to ship-by-accident.
    board_self_id_verify_or_halt();
    delay(300);
    Serial.println("{\"ready\":true}");

    cfg::init();
    settings::init();

    // Factory reset: BOOT button (GPIO 0, also the SPACE/voice-mode key)
    // held at power-on. Sample for ~5s; if held the whole time, wipe NVS.
    {
        pinMode(0, INPUT_PULLUP);
        bool held = true;
        for (int i = 0; i < 50; i++) {     // 50 * 100ms = 5s
            if (digitalRead(0) != LOW) { held = false; break; }
            delay(100);
        }
        if (held) {
            Serial.println("setup: BOOT held 5s — wiping config");
            cfg::wipe();
        }
    }

    bool prov_mode = !cfg::has_wifi();
    // NOTE: WiFi / provisioning bring-up is deliberately deferred to AFTER the
    // touch + BLE init below (see the block just before radio::reconcile_at_boot).
    // Starting WiFi here races its association interrupts against the GPIO ISR
    // allocation in touch_hal_init() (attachInterrupt → esp_intr_alloc →
    // heap_caps_malloc, dispatched onto the small ipc1 task stack via
    // esp_ipc_call), intermittently overflowing it — "Stack canary watchpoint
    // triggered (ipc1)". Bringing WiFi up last keeps that ISR-allocation
    // window interrupt-quiet.

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();   // takes over brightness (DISPLAY_DEFAULT_BRIGHTNESS) and starts the idle timer

    power_hal_init();
    imu_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // ---- WiFi/BLE single-radio bring-up (mutual exclusion) ----
    // CRITICAL ORDER: settle the radio decision FIRST, then initialise ONLY
    // the winning radio's stack. The old order — ble_init() unconditionally,
    // then wifi_client::begin(), then disable one in reconcile — left BOTH
    // stacks live transiently: ble_set_enabled(false) only stops advertising,
    // it does NOT tear down the BT controller. So the controller stayed up
    // while the WiFi STA connect ran, and the WiFi/BT coexistence layer
    // aborted (ESP_ERROR_CHECK in esp_coex_common_timer_setfn_wrapper) →
    // boot crash-loop → NVS wipe → device fell back to the provisioning QR.
    // Bringing up only the winner means coex is never engaged.
    //
    // reconcile_at_boot resolves the illegal both-on default (creds win),
    // persists the loser, and applies the runtime gates. Its BLE gate is safe
    // to call before ble_init() has run (guarded by s_ble_initialized in
    // ble.cpp). WiFi bring-up is still deferred to here (not early setup) to
    // keep WiFi association IRQs clear of the touch/BLE ISR-alloc window — see
    // the ipc1 stack-overflow note near prov_mode.
    radio::reconcile_at_boot(prov_mode);

    if (prov_mode) {
        Serial.println("setup: no config — entering provisioning");
        provisioning::begin();            // SoftAP owns WiFi; BLE never inits
    } else if (settings::radio_wifi_enabled()) {
        String ssid, pass;
        cfg::load_wifi(ssid, pass);
        wifi_client::begin(ssid, pass);
        net_daemon::init();
    } else if (settings::radio_ble_enabled()) {
        ble_init();
    }
    input_hal_init();

    pet_init();
    achievements_init();
    streak_init();
    recap_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_update_wifi();
    if (prov_mode) {
        ui_show_provisioning(provisioning::ap_ssid());
    } else {
        ui_show_screen(SCREEN_SPLASH);
    }
    // Pet events flash a transient toast on SCREEN_PET when it's the active screen.
    pet_set_event_cb(pet_screen_on_event);

    // Achievement unlock callback — flash the unlock name as a toast on
    // SCREEN_PET (no-op if not on the screen; widget is hidden via container)
    // and rely on achievements.cpp's existing internal serial log line (P1-5).
    achievements_set_unlock_cb([](const Achievement* a) {
        if (!a) return;
        char buf[48];
        snprintf(buf, sizeof(buf), "★ %s", a->name);
        pet_screen_flash_toast(buf);
    });

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

// parse_json may mutate the buffer in place; copy first so callers
// (net_daemon::latest() in particular) can hand us a stable read-only
// pointer without us corrupting their snapshot on the next call.
// Returns true if the JSON parsed successfully.
static bool apply_payload(const char* json) {
    static char scratch[2048];
    size_t n = strnlen(json, sizeof(scratch) - 1);
    memcpy(scratch, json, n);
    scratch[n] = '\0';

    if (!parse_json(scratch, &usage)) return false;
    Serial.printf("apply: parsed sessions=%u\n", (unsigned)sessions.count);
    for (uint8_t i = 0; i < sessions.count; i++) {
        const SessionInfo& s = sessions.items[i];
        Serial.printf("  [%u] id=%s name=%s c=%u state=%d age=%u\n",
            i, s.id, s.name, s.ctx_pct, (int)s.state, s.age_sec);
    }
    session_screen_update(current_sessions());
    // Streak: note Claude session arrival (idempotent — only flips flag once per day).
    if (sessions.count > 0) {
        streak_note_claude_session();
    }
    int g_before = usage_rate_group();
    usage_rate_sample(usage.session_pct);
    int g_after = usage_rate_group();
    if (g_after != g_before) {
        Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
            g_before, g_after, usage.session_pct);
        if (splash_is_active()) splash_pick_for_current_rate();
    }
    ui_update(&usage);
    pet_on_metrics(&usage, current_sessions());
    return true;
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

void loop() {
    i2c_health_tick();      // bounce the I2C master if it's been wedged

    // Serial commands always work, even during provisioning (cfg/wifi/set etc.).
    check_serial_cmd();

    if (provisioning::active()) {
        provisioning::tick();
        // LVGL still needs to render the SCREEN_PROVISIONING screen and
        // service its own timers — otherwise the panel would stay black
        // even though provisioning is active. Skip BLE/WiFi/net/IMU/pet
        // logic but keep the display alive.
        lv_timer_handler();
        return;
    }

    // OOBE auto-trigger: 60s after boot, if nothing has connected, show
    // the first-boot QR. Auto-revert to splash if any data source comes
    // online while OOBE is up.
    {
        uint32_t now = millis();
        if (!s_oobe_decided && (now - s_boot_ms) >= 60000) {
            bool ble_seen   = ble_has_received_data();
            bool has_wifi   = cfg::has_wifi();
            bool prov_up    = provisioning::active();
            if (!ble_seen && !has_wifi && !prov_up) {
                Serial.println("[oobe] no daemon, no wifi, no AP — showing OOBE");
                ui_show_screen(SCREEN_OOBE);
                s_oobe_auto_shown = true;
            }
            s_oobe_decided = true;
        }

        // Auto-hide only for OOBE screens raised by the real trigger (not sim).
        // A "sim oobe" on a device that already has config would be immediately
        // reverted otherwise, making testing impossible.
        if (s_oobe_auto_shown && ui_get_current_screen() == SCREEN_OOBE) {
            if (ble_has_received_data() || cfg::has_wifi() || provisioning::active()) {
                Serial.println("[oobe] data source came online — reverting to splash");
                ui_show_screen(SCREEN_SPLASH);
                s_oobe_auto_shown = false;
            }
        }
    }

    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    wifi_client::tick();
    // net_daemon's HTTPS poll runs on a dedicated Core-0 task (see
    // net_daemon::init); the loop just reads the snapshot below.
    {
        // WiFi path: apply iff the daemon has a new snapshot we haven't seen
        // yet. Uses the daemon's own poll timestamp rather than the shared
        // g_last_apply_ms so BLE pushes don't block WiFi updates (and vice
        // versa) — fixes the perceived 5-second lag on the sessions screen.
        static uint32_t last_netd_ts = 0;
        if (net_daemon::state_fresh()) {
            uint32_t nd_ts = net_daemon::last_apply_ts();
            if (nd_ts != last_netd_ts) {
                const char* p = net_daemon::latest();
                if (p && p[0] && apply_payload(p)) {
                    last_netd_ts = nd_ts;
                    g_last_apply_ms = millis();   // keep BLE side's reference up to date
                }
            }
        }
    }
    power_hal_tick();
    imu_hal_tick();
    splash_tick();
    pet_tick();
    retirement_screen_tick();
    streak_tick();
    achievements_tick();
    recap_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    // Phase 4: HID typing removed. All buttons route through the
    // screen-aware dispatcher in ui.cpp; wake-press swallowing happens
    // inside ui_input_event for all three buttons.
    //
    // Lock combo (2.16 only): LEFT+RIGHT pressed within 200ms toggles
    // ui_set_locked(). To make the combo cleanly detectable, LEFT/RIGHT
    // single-presses are queued for 200ms before dispatching; if the
    // other button arrives during that window, both are eaten and the
    // lock toggles instead. MID has no combo and dispatches immediately.
    {
        static bool primary_was = false;
        static bool secondary_was = false;
        static uint32_t primary_press_ts = 0;
        static uint32_t secondary_press_ts = 0;
        static bool primary_pending = false;
        static bool secondary_pending = false;

        const uint32_t COMBO_WIN_MS = 200;
        const bool has_secondary = (board_caps().button_count >= 2);

        bool primary_now   = input_hal_is_held(INPUT_BTN_PRIMARY);
        bool secondary_now = has_secondary && input_hal_is_held(INPUT_BTN_SECONDARY);
        uint32_t now_ms    = millis();

        // Edge detection → mark pending
        if (primary_now && !primary_was) {
            primary_pending  = true;
            primary_press_ts = now_ms;
        }
        primary_was = primary_now;

        if (has_secondary) {
            if (secondary_now && !secondary_was) {
                secondary_pending  = true;
                secondary_press_ts = now_ms;
            }
            secondary_was = secondary_now;
        }

        // Combo fire: both pending within the window → toggle lock, eat both
        if (has_secondary && primary_pending && secondary_pending) {
            long dt = (long)primary_press_ts - (long)secondary_press_ts;
            if (dt < 0) dt = -dt;
            if ((uint32_t)dt <= COMBO_WIN_MS) {
                primary_pending = false;
                secondary_pending = false;
                ui_toggle_lock();
            }
        }

        // Dispatch pending single-button after the combo window expires
        // without a partner press. When locked, the button does not
        // dispatch an action — instead it wakes the panel if asleep so
        // the user can see the locked splash. Unlock requires the combo.
        if (primary_pending && (now_ms - primary_press_ts) >= COMBO_WIN_MS) {
            if (ui_is_locked()) idle_consume_wake_press();
            else                ui_input_event(UI_BTN_LEFT, true);
            primary_pending = false;
        }
        if (secondary_pending && (now_ms - secondary_press_ts) >= COMBO_WIN_MS) {
            if (ui_is_locked()) idle_consume_wake_press();
            else                ui_input_event(UI_BTN_RIGHT, true);
            secondary_pending = false;
        }

        // MID has no combo. Wake-only while locked.
        if (power_hal_pwr_pressed()) {
            if (ui_is_locked()) idle_consume_wake_press();
            else                ui_input_event(UI_BTN_MID, true);
        }
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Refresh WiFi glyph every loop tick (state_fresh() is a cheap bool
    // check; the glyph itself is lazy-created and cheap to re-tint).
    ui_update_wifi();

    // Snapshot the BLE payload into a local buffer before parsing. The
    // NimBLE host task can overwrite rx_buf at any moment, so reading the
    // raw pointer mid-parse produced torn JSON.
    static char json_buf[BLE_BUF_SIZE];
    if (ble_snapshot_data(json_buf, sizeof(json_buf))) {
        if (apply_payload(json_buf)) {
            g_last_apply_ms = millis();
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
