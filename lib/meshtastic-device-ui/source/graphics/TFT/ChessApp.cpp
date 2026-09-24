#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------------------------
// CHESS - the board. The game and the engine live in src/TDeckChess.cpp and src/chess/.
//
// Jake, 2026-09-22: "local chess with a chess ai kinda thing with difficulty settings up to
// master". Measured on this chip the engine reaches about depth 6-7 in a couple of seconds,
// which is a decent club player - so the levels are named for how they play, not with a rating
// I have not earned the right to print.
//
// ⛔ LAYOUT IS RELATIVE TO THE CONTENT AREA, NOT THE SCREEN. tui_statusbar_reserve() adds 20px
// of top padding, so children start at y=0 and the usable height is 220, not 240. Placing
// things at y=26 "to clear the status bar" double-counts it and pushes the bottom row off the
// screen - which is exactly what happened to the Mail app earlier today.
//
// 8 squares of 24px = 192, leaving 124 across for the side panel and 28 down for the status.
// -----------------------------------------------------------------------------------------

extern "C" {
void tdeck_chess_new_game(void);
unsigned char tdeck_chess_piece_at(int sq64);
bool tdeck_chess_highlighted(int sq64);
int tdeck_chess_moves_from(int sq64, unsigned char *dests, int cap);
bool tdeck_chess_try_move(int from64, int to64);
bool tdeck_chess_player_turn(void);
bool tdeck_chess_thinking(void);
bool tdeck_chess_request_engine(void);
bool tdeck_chess_undo(void);
int tdeck_chess_result(void);
const char *tdeck_chess_status(void);
int tdeck_chess_level(void);
void tdeck_chess_set_level(int lv);
const char *tdeck_chess_level_name(void);
void chess_open(void);
void chess_idle_check(void);
}

namespace
{
const int kSq = 24;      // square size; 8 x 24 = 192 across
const int kBoardX = 2;   // leaves 2px either side of the board
const int kBoardY = 4;

lv_obj_t *screen = nullptr;
lv_obj_t *sqObj[64] = {nullptr};   // the squares
lv_obj_t *pcLbl[64] = {nullptr};   // the piece sitting on each, hidden when empty
lv_obj_t *statusLbl = nullptr;
lv_obj_t *levelLbl = nullptr;
lv_timer_t *tick = nullptr;

int selected = -1;                 // square the player has picked up, or -1
unsigned char dests[32];
int destN = 0;
bool wasThinking = false;

// Light and dark squares, and the two "something is happening here" tints.
const uint32_t kLight = 0xE8D4B0, kDark = 0x9A7248;
const uint32_t kLightSel = 0xC9D98A, kDarkSel = 0x8FA85A;

// ⭐ PIECES ARE LETTERS ON A DISC, and that is a considered choice rather than laziness.
// The fonts on this device have no chess glyphs and generating one is a bigger detour than the
// whole app. Letters alone do not work - white text is invisible on a light square - so each
// piece gets a filled circle behind it in its own colour, with the letter in the contrasting
// one. That reads unambiguously at 24px and costs one object per piece.
const char *kLetters = " PNBRQK";

int sqAt(int file, int rank) { return rank * 8 + file; }

void refreshBoard(void)
{
    for (int i = 0; i < 64; i++) {
        const int file = i % 8, rank = i / 8;
        const bool lightSq = ((file + rank) & 1) != 0;
        const bool lit = tdeck_chess_highlighted(i) || (selected >= 0 && i == selected);
        bool isDest = false;
        for (int d = 0; d < destN; d++)
            if (dests[d] == i)
                isDest = true;
        uint32_t c = lightSq ? kLight : kDark;
        if (lit || isDest)
            c = lightSq ? kLightSel : kDarkSel;
        lv_obj_set_style_bg_color(sqObj[i], lv_color_hex(c), LV_PART_MAIN);

        const unsigned char pc = tdeck_chess_piece_at(i);
        if (!pc) {
            lv_obj_add_flag(pcLbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int type = pc & 7;
        const bool white = ((pc >> 3) & 1) == 0;
        char txt[2] = {kLetters[type], 0};
        lv_label_set_text(pcLbl[i], txt);
        lv_obj_set_style_bg_color(pcLbl[i], lv_color_hex(white ? 0xFFFFFF : 0x202020), LV_PART_MAIN);
        lv_obj_set_style_text_color(pcLbl[i], lv_color_hex(white ? 0x202020 : 0xFFFFFF), LV_PART_MAIN);
        lv_obj_clear_flag(pcLbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (statusLbl)
        lv_label_set_text(statusLbl, tdeck_chess_status());
    if (levelLbl)
        lv_label_set_text(levelLbl, tdeck_chess_level_name());
}

void onSquare(lv_event_t *e)
{
    if (tdeck_chess_thinking() || tdeck_chess_result())
        return;
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (selected < 0) {
        destN = tdeck_chess_moves_from(i, dests, sizeof(dests));
        selected = destN ? i : -1;
        refreshBoard();
        return;
    }
    if (i == selected) { // tapping it again puts the piece back down
        selected = -1;
        destN = 0;
        tdeck_chess_moves_from(-1, dests, 0);
        refreshBoard();
        return;
    }
    if (tdeck_chess_try_move(selected, i)) {
        selected = -1;
        destN = 0;
        refreshBoard();
        // Let the board repaint with the player's move BEFORE the engine starts thinking -
        // otherwise the screen sits on the old position for the whole search and it looks
        // like the tap was ignored.
        lv_async_call([](void *) { tdeck_chess_request_engine(); }, nullptr);
        return;
    }
    // Not a legal destination: treat it as picking up a different piece instead of an error.
    destN = tdeck_chess_moves_from(i, dests, sizeof(dests));
    selected = destN ? i : -1;
    refreshBoard();
}

lv_obj_t *sideButton(const char *txt, int y, uint32_t colour, lv_event_cb_t cb, lv_obj_t **lblOut = nullptr)
{
    lv_obj_t *b = lv_btn_create(screen);
    lv_obj_set_size(b, 116, 28);
    lv_obj_set_pos(b, 200, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    if (lblOut)
        *lblOut = l;
    return b;
}
} // namespace

extern "C" void chess_open(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x101010), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen);

        for (int i = 0; i < 64; i++) {
            const int file = i % 8, rank = i / 8;
            lv_obj_t *s = lv_obj_create(screen);
            lv_obj_remove_style_all(s);
            // rank 0 is White's back rank and must be at the BOTTOM of the screen, so the row
            // is flipped when positioning. Getting this wrong gives a board that plays
            // correctly and looks upside down.
            lv_obj_set_pos(s, kBoardX + file * kSq, kBoardY + (7 - rank) * kSq);
            lv_obj_set_size(s, kSq, kSq);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s, onSquare, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            sqObj[i] = s;

            lv_obj_t *p = lv_label_create(s);
            lv_obj_set_size(p, 20, 20);
            lv_obj_center(p);
            lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_style_pad_top(p, 3, LV_PART_MAIN);
            lv_obj_set_style_text_font(p, &ui_font_montserrat_12, LV_PART_MAIN);
            lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
            pcLbl[i] = p;
        }

        lv_obj_t *lvlLbl = lv_label_create(screen);
        lv_obj_set_pos(lvlLbl, 200, 4);
        lv_obj_set_size(lvlLbl, 116, 14);
        lv_label_set_long_mode(lvlLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(lvlLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(lvlLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_label_set_text(lvlLbl, "");
        levelLbl = lvlLbl;

        sideButton("Level", 22, 0x3a3a3c, [](lv_event_t *) {
            tdeck_chess_set_level((tdeck_chess_level() + 1) % 6);
            refreshBoard();
        });
        sideButton("New game", 56, 0x0a84ff, [](lv_event_t *) {
            tdeck_chess_new_game();
            selected = -1;
            destN = 0;
            refreshBoard();
        });
        sideButton("Take back", 90, 0x3a3a3c, [](lv_event_t *) {
            tdeck_chess_undo();
            selected = -1;
            destN = 0;
            refreshBoard();
        });

        statusLbl = lv_label_create(screen);
        lv_obj_set_pos(statusLbl, 200, 126);
        // ⛔ Bounded. The status carries engine output of unpredictable length ("e2e4 depth 6
        // +0.3", "Checkmate - you lose"), and a wrapping label with no height walks over
        // whatever is below it.
        lv_obj_set_size(statusLbl, 116, 70);
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(statusLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(statusLbl, "");

        // Repaint when the engine finishes. It moves on the main loop, so the UI has to notice
        // rather than be told - polling four times a second is cheap and cannot miss it.
        tick = lv_timer_create(
            [](lv_timer_t *) {
                const bool t = tdeck_chess_thinking();
                if (t != wasThinking) {
                    wasThinking = t;
                    refreshBoard();
                }
            },
            250, nullptr);
    }
    selected = -1;
    destN = 0;
    refreshBoard();
    lv_screen_load(screen);
}

// Hand the board's memory back once it has been left alone, exactly as Gemini does. 64 squares
// and 64 piece labels is the largest object count of any app here, so this one matters most.
extern "C" void chess_idle_check(void)
{
    static uint32_t idleSince = 0;
    if (!screen)
        return;
    if (lv_screen_active() == screen || tdeck_chess_thinking()) {
        idleSince = 0;
        return;
    }
    if (!idleSince) {
        idleSince = lv_tick_get();
        return;
    }
    if (lv_tick_get() - idleSince < 5000)
        return;
    idleSince = 0;
    if (tick) {
        lv_timer_delete(tick);
        tick = nullptr;
    }
    lv_obj_delete(screen); // takes the squares and piece labels with it
    screen = nullptr;
    statusLbl = levelLbl = nullptr;
    memset(sqObj, 0, sizeof(sqObj));
    memset(pcLbl, 0, sizeof(pcLbl));
    // ⭐ The GAME is not reset - only the screen. Come back and the position is exactly as you
    // left it, which is the whole point of a chess game you play across a day.
}
