// -----------------------------------------------------------------------------
// T-UI Nodes & Favorites — two launcher apps over one list.
//
// Jake, 2026-09-10: "can we make a favorites app as well? which means we need a
// way to favorite them .. which means we need a separate node list app. trying to
// keep the meshtastic app modified the least possible so that updates are easier
// in the future."
//
// That constraint shapes the whole design. Neither app touches MUI's own node
// panels. They are ordinary launcher screens built from scratch, fed by the
// extern "C" bridge in src/TDeckNodesBridge.cpp — the same arrangement the GPS
// readout already uses, because this library cannot include firmware headers. A
// future device-ui update merges without ever meeting this code.
//
// ONE LIST, TWO DOORS. Nodes shows everything and lets you tap the star;
// Favorites shows the same rows filtered to the starred ones. The bridge does the
// ordering (favourites first, then most-recently-heard), so the two screens cannot
// drift apart — Favorites is literally the same list with a filter.
//
// The star is Meshtastic's own favourite bit, not a file of ours, so a node
// starred here is starred in the phone app too and survives a reboot.
// -----------------------------------------------------------------------------
#include "lvgl.h"
#include <Arduino.h>
#include <cmath>
#include <cstdio>

extern "C" void nodes_open(void);
extern "C" void favorites_open(void);

// --- firmware bridge (src/TDeckNodesBridge.cpp, src/TDeckGpsBridge.cpp) ---
extern "C" int tdeck_nodes_list(uint32_t *out, int maxN);
extern "C" const char *tdeck_node_name(uint32_t num);
extern "C" uint32_t tdeck_node_age_secs(uint32_t num);
extern "C" int tdeck_node_battery(uint32_t num);
extern "C" bool tdeck_node_position(uint32_t num, int32_t *latI, int32_t *lonI);
extern "C" bool tdeck_node_is_favorite(uint32_t num);
extern "C" void tdeck_node_set_favorite(uint32_t num, bool on);
extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon);

namespace
{
// A busy mesh holds hundreds of nodes. Building one row widget per node is exactly
// what crash-looped the Max's first admin app, so this is capped rather than left
// to grow with the mesh.
const int kMaxRows = 60;

lv_obj_t *screen = nullptr;
lv_obj_t *listCont = nullptr;
lv_obj_t *titleLbl = nullptr;
lv_obj_t *emptyLbl = nullptr;
lv_obj_t *prevScreen = nullptr;
bool favOnly = false;
uint32_t rowNode[kMaxRows];

// "2m" / "3h" / "4d" — how long since we last heard from them.
void ageText(uint32_t secs, char *out, size_t n)
{
    if (!secs) {
        out[0] = 0;
        return;
    }
    if (secs < 3600)
        snprintf(out, n, "%um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, n, "%uh", (unsigned)(secs / 3600));
    else
        snprintf(out, n, "%ud", (unsigned)(secs / 86400));
}

// "1.2 km NE" — empty when either end has no position.
void distText(uint32_t num, char *out, size_t n)
{
    out[0] = 0;
    int32_t ourLat, ourLon, theirLat, theirLon;
    if (!tdeck_gps_position(&ourLat, &ourLon))
        return;
    if (!tdeck_node_position(num, &theirLat, &theirLon))
        return;
    const double a = ourLat * 1e-7 * M_PI / 180.0, b = ourLon * 1e-7 * M_PI / 180.0;
    const double c = theirLat * 1e-7 * M_PI / 180.0, d = theirLon * 1e-7 * M_PI / 180.0;
    const double dLat = c - a, dLon = d - b;
    const double h = sin(dLat / 2) * sin(dLat / 2) + cos(a) * cos(c) * sin(dLon / 2) * sin(dLon / 2);
    const double m = 6371000.0 * 2 * atan2(sqrt(h), sqrt(1 - h));
    const double y = sin(dLon) * cos(c), x = cos(a) * sin(c) - sin(a) * cos(c) * cos(dLon);
    double brg = atan2(y, x) * 180.0 / M_PI;
    if (brg < 0)
        brg += 360.0;
    static const char *kPts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const char *pt = kPts[(int)((brg + 22.5) / 45.0) % 8];
    if (m < 1000)
        snprintf(out, n, "%d m %s", (int)m, pt);
    else
        snprintf(out, n, "%.1f km %s", m / 1000.0, pt);
}

void rebuild(void);

// Tapping the star toggles it, then the list is REBUILT rather than patched: on
// Favorites an un-starred row has to leave, and on Nodes a starred one re-sorts to
// the top. Both fall out of a rebuild for free.
void onStar(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= kMaxRows)
        return;
    const uint32_t num = rowNode[idx];
    tdeck_node_set_favorite(num, !tdeck_node_is_favorite(num));
    rebuild();
}

void addRow(lv_obj_t *parent, uint32_t num, int idx)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, tdeck_node_name(num));
    lv_obj_set_style_text_color(name, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 200);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 8, 4);

    char age[12], dist[24], batt[12], detail[72];
    ageText(tdeck_node_age_secs(num), age, sizeof(age));
    distText(num, dist, sizeof(dist));
    const int b = tdeck_node_battery(num);
    if (b < 0)
        batt[0] = 0;
    else if (b > 100)
        snprintf(batt, sizeof(batt), "USB"); // Meshtastic sends >100 for "not on a battery"
    else
        snprintf(batt, sizeof(batt), "%d%%", b);
    // Anything the node has not told us is left out rather than shown as a zero.
    snprintf(detail, sizeof(detail), "%s%s%s%s%s", dist, (dist[0] && batt[0]) ? "  ·  " : "", batt,
             ((dist[0] || batt[0]) && age[0]) ? "  ·  seen " : (age[0] ? "seen " : ""), age);
    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, detail[0] ? detail : "nothing heard yet");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 8, -4);

    // The star gets its own 44px tap target at the right edge, so it can never be
    // mistaken for a tap on the row itself.
    lv_obj_t *star = lv_obj_create(row);
    lv_obj_remove_style_all(star);
    lv_obj_set_size(star, 44, 44);
    lv_obj_align(star, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(star, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(star, onStar, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *sl = lv_label_create(star);
    const bool on = tdeck_node_is_favorite(num);
    lv_label_set_text(sl, on ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(sl, lv_color_hex(on ? 0xffd60a : 0x5a5a5e), LV_PART_MAIN);
    lv_obj_center(sl);
}

void rebuild(void)
{
    lv_obj_clean(listCont);
    static uint32_t all[kMaxRows];
    const int n = tdeck_nodes_list(all, kMaxRows);
    int shown = 0;
    for (int i = 0; i < n && shown < kMaxRows; i++) {
        if (favOnly && !tdeck_node_is_favorite(all[i]))
            continue;
        rowNode[shown] = all[i];
        addRow(listCont, all[i], shown);
        shown++;
    }
    lv_label_set_text(titleLbl, favOnly ? "Favorites" : "Nodes");
    if (shown == 0) {
        lv_label_set_text(emptyLbl, favOnly ? "No favorites yet." : "No nodes heard yet.");
        lv_obj_clear_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
    }
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_obj_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 6, LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        back,
        [](lv_event_t *) {
            if (prevScreen)
                lv_screen_load_anim(prevScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(bl);

    titleLbl = lv_label_create(screen);
    lv_label_set_text(titleLbl, "Nodes");
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 9);

    listCont = lv_obj_create(screen);
    lv_obj_remove_style_all(listCont);
    lv_obj_set_size(listCont, 312, 198);
    lv_obj_align(listCont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_flex_flow(listCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont, LV_DIR_VER);

    emptyLbl = lv_label_create(screen);
    lv_label_set_text(emptyLbl, "");
    lv_obj_set_style_text_color(emptyLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(emptyLbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(emptyLbl, LV_OBJ_FLAG_HIDDEN);
}

void openWith(bool favourites)
{
    if (!screen)
        buildScreen();
    lv_obj_t *active = lv_screen_active();
    if (active != screen)
        prevScreen = active; // remember the launcher, never our own screen
    favOnly = favourites;
    rebuild();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
} // namespace

extern "C" void nodes_open(void)
{
    openWith(false);
}

extern "C" void favorites_open(void)
{
    openWith(true);
}
