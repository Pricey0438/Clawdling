#pragma once
#include <lvgl.h>

// Reusable discrete-paging chrome shared by the settings grid, the Past Pets
// gallery, and the Achievements screen. Provides two invisible full-height
// gutter tap-zones (each with a dim chevron hint) flanking the content, plus a
// row of page dots near the bottom. Stepping fires on_change(new_page); the
// owner repaints its page content. NO scrolling, NO slide animation — each
// step is one bounded redraw, which is what stays fast on the CO5300 panel.
struct PagedNav {
    lv_obj_t* parent;
    lv_obj_t* left_zone;
    lv_obj_t* right_zone;
    lv_obj_t* dots_row;
    int       page;
    int       page_count;
    void (*on_change)(int page);
};

// Build gutter zones + dot row into `parent`. `gutter_w` = side-column width
// (52 on the 2.16). Zones span vertical [top, parent_h - bottom_inset].
// Call once. Dots start at a single page; call paged_nav_set_page_count next.
void paged_nav_create(PagedNav* nav, lv_obj_t* parent, int gutter_w,
                      int top, int bottom_inset, void (*on_change)(int));

// Set page count (rebuilds the dot row, clamps current page). Hides the whole
// nav (gutters + dots) when count <= 1, so single-page views show no chrome.
void paged_nav_set_page_count(PagedNav* nav, int count);

// Jump to `page` (clamped to [0,count-1]); recolors dots and fires on_change.
void paged_nav_goto(PagedNav* nav, int page);

// Step by delta (+1/-1), wrapping at the ends.
void paged_nav_step(PagedNav* nav, int delta);
