#include "session.h"
#include "splash.h"
#include "theme.h"
#include "ui.h"
#include "ui_panels.h"
#include "pet.h"            // current pet's shiny flag tints the detail animation
#include "hal/board_caps.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Fonts come in via theme.h's theme_font_* helpers (display / display_s / body / label).

enum view_t { VIEW_LIST, VIEW_DETAIL };
static view_t current_view = VIEW_LIST;
static int8_t focused_idx = -1;
// Stable identity of the focused session. Matches SessionInfo::id (8 chars +
// null). Captured in row_tap_cb and re-bound by id every session_screen_update
// so a list shrink that removes earlier entries can't silently switch the
// detail view to a different session at the same index.
static char focused_id[9] = {0};

// Module-owned widget pointers. `root` is the session_container passed by
// ui.cpp's init_session_screen() — we build everything inside it rather
// than create a new screen, matching the codebase's existing pattern.
static lv_obj_t* root = nullptr;
static lv_obj_t* list_obj = nullptr;
static lv_obj_t* empty_lbl = nullptr;

// Detail view root. Built lazily on first entry to avoid a large PSRAM
// allocation at boot; reset_view() / update() null-check before touching.
static lv_obj_t* detail_obj = nullptr;

// Cached most-recent list so detail-view (Task 17) can read after a tap.
static SessionList cache = {};

// P2-5: snapshot of the (count + ids) that were last reflected in the LVGL
// list view, so session_screen_update can skip rebuild_list() when the
// session set hasn't changed. We deliberately compare ONLY count + ids here
// (not ctx_pct / state / age) — the audit's call-out is the LVGL widget tree
// teardown, which is structural. Accepted tradeoff: while the session set is
// stable, per-row ctx_pct / state pill / age won't refresh on the list view
// (they still refresh whenever ids change OR when the user enters the detail
// view). Adding a per-row no-rebuild refresh path was explicitly scoped out.
// -1 in last_rebuilt_count means "never built".
static int  last_rebuilt_count = -1;
static char last_rebuilt_ids[SESSION_MAX][9] = {{0}};

static bool list_structurally_unchanged(const SessionList* list) {
    if (last_rebuilt_count != (int)list->count) return false;
    for (uint8_t i = 0; i < list->count; i++) {
        if (strncmp(last_rebuilt_ids[i], list->items[i].id,
                    sizeof(last_rebuilt_ids[i])) != 0) return false;
    }
    return true;
}

static void snapshot_rebuilt_ids(const SessionList* list) {
    last_rebuilt_count = (int)list->count;
    for (uint8_t i = 0; i < SESSION_MAX; i++) {
        if (i < list->count) {
            strncpy(last_rebuilt_ids[i], list->items[i].id,
                    sizeof(last_rebuilt_ids[i]) - 1);
            last_rebuilt_ids[i][sizeof(last_rebuilt_ids[i]) - 1] = '\0';
        } else {
            last_rebuilt_ids[i][0] = '\0';
        }
    }
}

// Phase 4 — keep row widget pointers so the focus visual can be
// re-applied without a full rebuild. Indexed by session position.
static lv_obj_t* session_rows[SESSION_MAX] = {0};
// Per-row dynamic widgets captured during rebuild_list so realtime updates
// (ctx %, state pill, bar color) can refresh without a full LVGL teardown
// even when the session set is structurally unchanged.
static lv_obj_t* row_pct_lbl[SESSION_MAX]   = {0};
static lv_obj_t* row_state_lbl[SESSION_MAX] = {0};
static lv_obj_t* row_bar[SESSION_MAX]       = {0};
static lv_obj_t* row_name_lbl[SESSION_MAX]  = {0};

// Guard against re-entrant focus refreshes during rebuild_list(). lv_obj_clean
// dispatches LV_EVENT_DELETE on each child and LV_EVENT_CHILD_CHANGED /
// LV_EVENT_CHILD_DELETED on the parent. If any of those handlers triggers
// refresh_session_focus_visual (directly or indirectly) it would see a
// transient state: session_rows[] are NULL'd at the top of rebuild_list, but
// cache.count still reflects the soon-to-be-populated row count. The NULL
// check inside refresh_session_focus_visual handles that case correctly, but
// we also short-circuit early to avoid touching widgets whose deletion is
// still in flight.
static bool rebuilding_list = false;

// Animation state for the detail view
static uint16_t det_anim_idx = 0;
static uint16_t det_frame_idx = 0;
static uint32_t det_last_frame_ms = 0;
static uint32_t det_last_pick_ms = 0;
static session_state_t det_last_mood = SESSION_IDLE;
#define DET_FRAME_MS  120
#define DET_PICK_MS   20000

// Detail-view widget pointers
static lv_obj_t* detail_canvas = nullptr;
static lv_obj_t* detail_name_lbl = nullptr;
static lv_obj_t* detail_ctx_bar = nullptr;
static lv_obj_t* detail_ctx_lbl = nullptr;
static lv_obj_t* detail_state_lbl = nullptr;
static uint16_t* detail_canvas_buf = nullptr;
static int detail_cell = 12;

static uint8_t group_for_state(session_state_t s) {
    switch (s) {
        case SESSION_THINKING:   return 3;  // heavy group (dj dances)
        case SESSION_COMPACTING: return 1;  // "work think" / "work coding"
        case SESSION_WAITING:    return 0;  // idle/sleepy group
        default:                 return 0;
    }
}

static void detail_canvas_tap_cb(lv_event_t* e) {
    // Tap on the animation area returns to list view.
    session_screen_reset_view();
}

static void detail_build_once() {
    if (detail_obj) return;
    const BoardCaps& caps = board_caps();
    // Layout breakpoints mirror the rest of the UI: large (H>=460) vs compact.
    // anim_cell 11 (large) / 9 (compact). Each board's cell got shrunk by one
    // pixel to give the animation bounce room above the status bar — the
    // headphones reach grid row 0 on peak frames and clipped under the
    // divider with the previous values.
    int anim_cell = caps.height >= 460 ? 11 : 9;
    int panel_h   = caps.height >= 460 ? 130 : 110;
    int canvas_w  = 20 * anim_cell;
    int canvas_h  = 20 * anim_cell;

    // Allocate the canvas backing buffer FIRST. If PSRAM is exhausted, bail
    // before creating any LVGL widgets — the next tap can retry cleanly and
    // the list view stays usable. detail_obj remaining null signals failure
    // to the caller (session_screen_update falls back to list).
    //
    // Defensive: if a previous build left a buffer around without a matching
    // detail_obj (shouldn't happen given the reset_view ordering, but cheap
    // to guard against), free it before re-allocating to avoid a leak.
    if (detail_canvas_buf) {
        heap_caps_free(detail_canvas_buf);
        detail_canvas_buf = nullptr;
    }
    detail_canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, MALLOC_CAP_SPIRAM);
    if (!detail_canvas_buf) {
        Serial.println("session: detail canvas alloc failed (PSRAM exhausted)");
        return;
    }

    detail_obj = lv_obj_create(root);
    lv_obj_remove_style_all(detail_obj);
    lv_obj_set_size(detail_obj, caps.width, caps.height);
    lv_obj_set_style_bg_color(detail_obj, THEME_BG, 0);
    lv_obj_add_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);

    detail_canvas = lv_canvas_create(detail_obj);
    // Strip lv_obj defaults — the canvas displays its buffer pixel-for-pixel,
    // but a leftover 2-3px default pad/border was offsetting the canvas top
    // into the status-bar divider on AMOLED-2.16, clipping the first row of
    // pixel-art cells.
    lv_obj_remove_style_all(detail_canvas);
    lv_obj_set_size(detail_canvas, canvas_w, canvas_h);
    lv_canvas_set_buffer(detail_canvas, detail_canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    // Reserve status-bar header (sb_y + sbh + divider) plus a small breathing
    // gap, then bias the canvas toward the bottom of the available space so
    // the top has plenty of room for animation bounce. A naive centre puts
    // the top gap and bottom gap equal — fine for static art, but the splash
    // creatures bounce 10-20px and can clip into the status bar divider on
    // peak frames if the top gap is small. 2:1 bias gives the top twice the
    // breathing room of the bottom.
    int sbh = theme_status_bar_h();
    int header_h = SP_XS + sbh + 1 + SP_S;   // sb_y + bar + divider + breathing gap
    int avail    = caps.height - header_h - panel_h - 20;
    int slack    = avail - canvas_h;
    if (slack < 0) slack = 0;
    int canvas_y = header_h + (slack * 2) / 3;
    lv_obj_align(detail_canvas, LV_ALIGN_TOP_MID, 0, canvas_y);
    lv_obj_add_flag(detail_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(detail_canvas, detail_canvas_tap_cb, LV_EVENT_CLICKED, nullptr);
    detail_cell = anim_cell;

    // Status panel anchored at bottom
    lv_obj_t* panel = lv_obj_create(detail_obj);
    lv_obj_set_size(panel, caps.width - 40, panel_h);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(panel, THEME_PANEL, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, SP_M, 0);

    detail_name_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(detail_name_lbl, theme_font_display_s(), 0);
    lv_obj_set_style_text_color(detail_name_lbl, THEME_TEXT, 0);
    lv_obj_align(detail_name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    detail_ctx_bar = lv_bar_create(panel);
    lv_obj_set_size(detail_ctx_bar, caps.width - 80, 10);
    lv_bar_set_range(detail_ctx_bar, 0, 100);
    lv_obj_set_style_bg_color(detail_ctx_bar, THEME_BAR_BG, 0);
    lv_obj_align(detail_ctx_bar, LV_ALIGN_LEFT_MID, 0, 0);

    detail_ctx_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(detail_ctx_lbl, theme_font_body(), 0);
    lv_obj_set_style_text_color(detail_ctx_lbl, THEME_TEXT, 0);
    lv_obj_align(detail_ctx_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    detail_state_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(detail_state_lbl, theme_font_body(), 0);
    lv_obj_align(detail_state_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void detail_render_frame() {
    if (!detail_canvas_buf) return;
    int canvas_w = 20 * detail_cell;
    const Pet* p = pet_current();
    bool shiny = (p && p->is_shiny);
    splash_render_to_buf(detail_canvas_buf, canvas_w, detail_cell,
                         det_anim_idx, det_frame_idx, shiny);
    lv_obj_invalidate(detail_canvas);
}

static int row_height(int h) {
    // Match USAGE: 150px on large screens (AMOLED-2.16), 130px otherwise.
    return h >= 460 ? 150 : 130;
}

static const char* state_text(session_state_t s) {
    switch (s) {
        case SESSION_THINKING:   return "thinking";
        case SESSION_WAITING:    return "waiting";
        case SESSION_COMPACTING: return "compacting";
        default:                 return "idle";
    }
}

static lv_color_t state_color(session_state_t s) {
    switch (s) {
        case SESSION_THINKING:   return THEME_AMBER;
        case SESSION_WAITING:    return THEME_GREEN;
        case SESSION_COMPACTING: return THEME_PURPLE;
        default:                 return THEME_DIM;
    }
}

// Push the SessionInfo into the detail-view label widgets. Cheap — LVGL skips
// invalidates when the string/value/color is unchanged, so calling this every
// tick from session_screen_tick_anim is a no-op in steady state. The reason
// it's also called per-tick (not just per-payload): some payloads from the
// daemon omit `ses` entirely (usage-only refresh) or repeat the prior value
// for ctx_pct / state while the user is mid-prompt, and we want the detail
// view to stay in lockstep with whatever cache.items[focused_idx] holds.
static void detail_apply_labels(const SessionInfo& s) {
    if (!detail_obj || !detail_name_lbl) return;
    lv_label_set_text(detail_name_lbl, s.name);
    lv_bar_set_value(detail_ctx_bar, s.ctx_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(detail_ctx_bar, state_color(s.state), LV_PART_INDICATOR);
    lv_label_set_text_fmt(detail_ctx_lbl, "%u%%", s.ctx_pct);
    lv_label_set_text(detail_state_lbl, state_text(s.state));
    lv_obj_set_style_text_color(detail_state_lbl, state_color(s.state), 0);
}

// Apply the 4px amber left-border to the focused session row, clear it
// from others. Called whenever focused_idx changes or list rebuilds.
static void refresh_session_focus_visual(void) {
    // Mid-rebuild the row pointers are either NULL'd or about to be replaced.
    // rebuild_list() always calls us itself when it finishes, so bailing here
    // is correct, not a missed update.
    if (rebuilding_list) return;
    for (uint8_t i = 0; i < cache.count && i < SESSION_MAX; i++) {
        if (!session_rows[i]) continue;
        bool focused = ((int8_t)i == focused_idx);
        lv_obj_set_style_border_width(session_rows[i], focused ? 4 : 0, 0);
    }
}

// Drill from the list view into the detail view for the given index.
// Shared by row_tap_cb (touch fallback on AMOLED-1.8) and session_on_button
// (R press on AMOLED-2.16) so both paths stay in sync.
static void drill_into(int idx) {
    if (idx < 0 || idx >= cache.count) return;
    focused_idx = (int8_t)idx;
    strlcpy(focused_id, cache.items[idx].id, sizeof(focused_id));
    current_view = VIEW_DETAIL;
    ui_status_bar_set_title("< SESSIONS");
    ui_status_bar_set_back_cb(session_screen_reset_view);

    // Hide list, show detail. detail_obj may not exist yet on first entry —
    // session_screen_update() will build it and clear its HIDDEN flag.
    if (list_obj)   lv_obj_add_flag(list_obj,   LV_OBJ_FLAG_HIDDEN);
    if (empty_lbl)  lv_obj_add_flag(empty_lbl,  LV_OBJ_FLAG_HIDDEN);
    if (detail_obj) lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);

    // After a tap-driven drill, LVGL's indev still has the pressed row as
    // its current target. Clear it so the next press starts fresh on the
    // detail view, not on whatever row the user just tapped.
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);

    session_screen_update(&cache);
}

static void row_tap_cb(lv_event_t* e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= cache.count) return;
    focused_idx = (int8_t)idx;
    refresh_session_focus_visual();
    drill_into((int)idx);
}

static void build_list_row(lv_obj_t* parent, int idx, const SessionInfo& s, int row_h, int width) {
    // Outer wrapper: holds the focus border, captures row-tap on AMOLED-1.8.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width - 2 * SP_XL, row_h);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_color(row, THEME_AMBER, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (idx < SESSION_MAX) session_rows[idx] = row;
    // Touch is additive on both boards — buttons keep working unchanged.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_tap_cb, LV_EVENT_PRESSED, (void*)(intptr_t)idx);

    // USAGE-style card inside the wrapper. y=0 because the wrapper owns position.
    lv_obj_t *pct = nullptr, *pill = nullptr, *bar = nullptr, *reset = nullptr;
    make_usage_panel(row, 0, s.name, &pct, &pill, &bar, &reset);

    // make_usage_panel positions the panel at (L.margin, 0) inside the parent
    // — perfect for USAGE which calls it with the screen container, but here
    // the row wrapper is already L.content_w wide so the panel must sit at x=0
    // or it overflows the wrapper and gets clipped on the right.
    lv_obj_t* panel = lv_obj_get_parent(pct);
    lv_obj_set_x(panel, 0);

    lv_label_set_text_fmt(pct, "%u%%", (unsigned)s.ctx_pct);
    lv_bar_set_value(bar, s.ctx_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, state_color(s.state), LV_PART_INDICATOR);

    lv_label_set_text(reset, state_text(s.state));
    lv_obj_set_style_text_color(reset, state_color(s.state), 0);

    // Stash dynamic widgets for refresh_list_values — values can change while
    // the session set itself is structurally unchanged (e.g. ctx % tick,
    // working → waiting transition).
    if (idx < SESSION_MAX) {
        row_pct_lbl[idx]   = pct;
        row_state_lbl[idx] = reset;
        row_bar[idx]       = bar;
        // name comes back via make_usage_panel as the first label child of the
        // panel; capture by walking children rather than threading another out
        // pointer through the helper signature.
        row_name_lbl[idx]  = nullptr;
        uint32_t n = lv_obj_get_child_count(panel);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* c = lv_obj_get_child(panel, i);
            if (lv_obj_check_type(c, &lv_label_class) && c != pct && c != reset) {
                row_name_lbl[idx] = c;
                break;
            }
        }
    }
}

// Realtime refresh — push the latest values into existing row widgets when
// the session set itself hasn't structurally changed. Cheap: LVGL skips
// invalidates when the string/value/color is unchanged.
static void refresh_list_values(const SessionList* list) {
    for (uint8_t i = 0; i < list->count && i < SESSION_MAX; i++) {
        const SessionInfo& s = list->items[i];
        if (row_pct_lbl[i])   lv_label_set_text_fmt(row_pct_lbl[i], "%u%%", (unsigned)s.ctx_pct);
        if (row_bar[i]) {
            lv_bar_set_value(row_bar[i], s.ctx_pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(row_bar[i], state_color(s.state), LV_PART_INDICATOR);
        }
        if (row_state_lbl[i]) {
            lv_label_set_text(row_state_lbl[i], state_text(s.state));
            lv_obj_set_style_text_color(row_state_lbl[i], state_color(s.state), 0);
        }
        if (row_name_lbl[i]) lv_label_set_text(row_name_lbl[i], s.name);
    }
}

static void rebuild_list() {
    if (!list_obj) return;
    // Phase 4 — old row pointers become dangling after lv_obj_clean. NULL
    // them first so the focus-visual guard sees a consistent empty state if
    // lv_obj_clean's delete events fire any user code that calls back in.
    rebuilding_list = true;
    for (int i = 0; i < SESSION_MAX; i++) {
        session_rows[i]   = nullptr;
        row_pct_lbl[i]    = nullptr;
        row_state_lbl[i]  = nullptr;
        row_bar[i]        = nullptr;
        row_name_lbl[i]   = nullptr;
    }
    lv_obj_clean(list_obj);

    if (cache.count == 0) {
        if (!empty_lbl) {
            empty_lbl = lv_label_create(root);
            lv_label_set_text(empty_lbl, "No active sessions");
            lv_obj_set_style_text_color(empty_lbl, THEME_DIM, 0);
            lv_obj_set_style_text_font(empty_lbl, theme_font_display_s(), 0);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        focused_idx = -1;
        rebuilding_list = false;
        return;
    }
    if (empty_lbl) lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);

    const BoardCaps& caps = board_caps();
    int row_h = row_height(caps.height);
    for (uint8_t i = 0; i < cache.count; i++) {
        build_list_row(list_obj, i, cache.items[i], row_h, caps.width);
    }

    // Phase 4 — clamp focus and refresh visual.
    if (focused_idx < 0) focused_idx = 0;
    if (focused_idx >= cache.count) focused_idx = cache.count - 1;
    rebuilding_list = false;
    refresh_session_focus_visual();
}

void session_screen_init(lv_obj_t* parent) {
    root = parent;
    if (!root) return;

    const BoardCaps& caps = board_caps();

    // Scrollable column of session rows. Sits below the title and stops
    // short of the bottom edge to clear the AMOLED's rounded corners.
    list_obj = lv_obj_create(root);
    lv_obj_remove_style_all(list_obj);
    // Start at the same y as USAGE's first card (L.content_y: 100 large /
    // 85 compact) so the two screens rhyme — they share the make_usage_panel
    // card, and previously SESSIONS sat ~44px higher/tighter to the divider.
    int list_y = (caps.height >= 460) ? 100 : 85;
    lv_obj_set_size(list_obj, caps.width, caps.height - list_y - SP_XL);
    lv_obj_align(list_obj, LV_ALIGN_TOP_MID, 0, list_y);
    lv_obj_set_flex_flow(list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_obj,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list_obj, caps.height >= 460 ? 16 : 12, 0);  // match L.usage_panel_gap
    lv_obj_set_style_pad_top(list_obj, 0, 0);
    lv_obj_set_style_pad_bottom(list_obj, SP_M, 0);
    lv_obj_set_scroll_dir(list_obj, LV_DIR_VER);
    // Don't bubble row-area taps: ui.cpp deliberately omits the splash
    // toggle on session_container so accidental mis-taps near a row don't
    // jump to the splash screen. Splash is still reachable from Usage /
    // Bluetooth or via the PWR cycle.

    // Create the empty-state label up-front so SCREEN_SESSION isn't a
    // black void when the user navigates there before the daemon has
    // pushed any payload (or after the daemon disconnects). rebuild_list
    // will toggle its visibility as sessions arrive / leave.
    empty_lbl = lv_label_create(root);
    lv_label_set_text(empty_lbl, "No active sessions");
    lv_obj_set_style_text_color(empty_lbl, THEME_DIM, 0);
    lv_obj_set_style_text_font(empty_lbl, theme_font_display_s(), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
}

void session_screen_update(const SessionList* list) {
    if (!list) return;
    cache = *list;

    if (current_view == VIEW_LIST) {
        if (list_structurally_unchanged(list)) {
            // Same session set as last build — skip the LVGL teardown but
            // push fresh values into the existing row widgets so ctx %,
            // bar fill, and state pill stay live with each daemon payload.
            refresh_list_values(list);
            return;
        }
        rebuild_list();
        snapshot_rebuilt_ids(list);
    } else if (current_view == VIEW_DETAIL) {
        // Re-bind focus by session id, not index. A list shrink that removes
        // an earlier session would otherwise leave focused_idx pointing at a
        // different session at the same position. If the focused session is
        // gone entirely, fall back to list view.
        int8_t found = -1;
        for (uint8_t i = 0; i < cache.count; i++) {
            if (strcmp(cache.items[i].id, focused_id) == 0) { found = (int8_t)i; break; }
        }
        if (found < 0) {
            session_screen_reset_view();
            return;
        }
        focused_idx = found;
        detail_build_once();
        if (!detail_obj) {
            // PSRAM exhausted: detail view can't be built. Bail back to list
            // so the user isn't staring at a black screen.
            session_screen_reset_view();
            return;
        }
        // First-tap path: row_tap_cb's lv_obj_clear_flag was a no-op because
        // detail_obj didn't exist yet — make sure it's visible now.
        lv_obj_clear_flag(detail_obj, LV_OBJ_FLAG_HIDDEN);

        const SessionInfo& s = cache.items[focused_idx];
        detail_apply_labels(s);

        // If mood changed, pick a fresh animation immediately.
        if (s.state != det_last_mood) {
            det_last_mood = s.state;
            int idx = splash_pick_in_group(group_for_state(s.state), 0);
            if (idx >= 0) {
                det_anim_idx = (uint16_t)idx;
                det_frame_idx = 0;
                det_last_pick_ms = lv_tick_get();
            }
        }
        detail_render_frame();
    }
}

void session_screen_tick_anim(void) {
    if (current_view != VIEW_DETAIL) return;
    // focused_idx is re-bound by id in session_screen_update; this guard is
    // a belt-and-suspenders nil check covering the brief windows between
    // updates. The authoritative focus binding lives there, not here.
    if (focused_idx < 0 || focused_idx >= cache.count) return;

    // Re-apply labels from cache every tick so the % / state pill stay in
    // sync even when the daemon pushes a payload that doesn't trip
    // session_screen_update's DETAIL branch (e.g. usage-only refresh with
    // no `ses` field, or repeated values). lv_label_set_text is a no-op
    // when the string is unchanged, so this is free in steady state.
    detail_apply_labels(cache.items[focused_idx]);

    uint32_t now = lv_tick_get();

    // Auto-rotate within group every DET_PICK_MS
    if (now - det_last_pick_ms > DET_PICK_MS) {
        int idx = splash_pick_in_group(group_for_state(det_last_mood), (uint8_t)(now / 1000));
        if (idx >= 0 && (uint16_t)idx != det_anim_idx) {
            det_anim_idx = (uint16_t)idx;
            det_frame_idx = 0;
        }
        det_last_pick_ms = now;
    }
    // Frame advance
    if (now - det_last_frame_ms > DET_FRAME_MS) {
        det_frame_idx++;
        det_last_frame_ms = now;
        detail_render_frame();
    }
}

void session_screen_reset_view(void) {
    current_view = VIEW_LIST;
    ui_status_bar_set_title("SESSIONS");
    ui_status_bar_set_back_cb(nullptr);
    focused_idx = -1;
    focused_id[0] = '\0';

    // P2-7: lazy-free the detail view + its ~10 KB PSRAM canvas backing
    // buffer. detail_build_once() will rebuild both on the next drill-in.
    // Order matters: delete the LVGL widget tree first (so LVGL no longer
    // refers to detail_canvas_buf via lv_canvas_set_buffer), then free the
    // buffer. Null both pointers so a stray re-entry can't double-free.
    if (detail_obj) {
        lv_obj_delete(detail_obj);
        detail_obj = nullptr;
        // All child pointers (detail_canvas, detail_name_lbl, ...) become
        // dangling here — LVGL deleted them transitively. Null them so the
        // next detail_build_once() starts from a known-clean state.
        detail_canvas    = nullptr;
        detail_name_lbl  = nullptr;
        detail_ctx_bar   = nullptr;
        detail_ctx_lbl   = nullptr;
        detail_state_lbl = nullptr;
    }
    if (detail_canvas_buf) {
        heap_caps_free(detail_canvas_buf);
        detail_canvas_buf = nullptr;
    }

    if (list_obj)   lv_obj_clear_flag(list_obj, LV_OBJ_FLAG_HIDDEN);
    if (empty_lbl && cache.count == 0) lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
}

bool session_on_button(ui_btn_t btn) {
    // Detail sub-view: Mid backs out to list. L/R no-op (reserved).
    if (current_view == VIEW_DETAIL) {
        if (btn == UI_BTN_MID) {
            session_screen_reset_view();
            return true;
        }
        return false;
    }
    // List view.
    switch (btn) {
        case UI_BTN_LEFT: {
            if (cache.count <= 0) return true;
            if (focused_idx < 0) focused_idx = 0;
            else                 focused_idx = (focused_idx + 1) % cache.count;
            refresh_session_focus_visual();
            Serial.printf("session: L → focus=%d\n", (int)focused_idx);
            return true;
        }
        case UI_BTN_RIGHT: {
            if (cache.count <= 0 || focused_idx < 0) return true;
            Serial.printf("session: R → drill into session %d\n", (int)focused_idx);
            drill_into((int)focused_idx);
            return true;
        }
        case UI_BTN_MID:
            return false;   // let dispatcher cycle screens
    }
    return false;
}
