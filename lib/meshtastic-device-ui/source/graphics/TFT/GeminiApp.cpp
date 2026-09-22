// -----------------------------------------------------------------------------
// Gemini for the colour T-Deck — the screen. The network lives in src/TDeckGemini.cpp.
//
// Jake: "Could you make a Gemini app for that?"
//
// Type a question, press Ask (or Enter), read the answer. That is the whole app, and
// deliberately so: this is a handheld with a thumb keyboard, and every control added is another
// thing to hit by accident while typing.
//
// ⚠️ ASKING DROPS BLUETOOTH until the next reboot — one antenna, and starting wi-fi tears BT
// down. Jake accepted that ("I won't Bluetooth much to my tdeck") but the screen says so anyway,
// because losing a phone link with no explanation is the kind of thing that gets blamed on a
// bug three weeks later.
//
// THREADING: nothing here touches the network. tdeckgemini::ask() records the question and the
// main loop does the work; this polls the state and repaints. The UI task must never block on a
// socket — the rule the whole launcher is built on.
// -----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>

extern "C" void gemini_open(void);

// src/TDeckGemini.cpp — the linker joins these across the src/ and lib/ boundary, the same trick
// the mesh kill-switch and the buzzer helpers use.
namespace tdeckgemini
{
enum State { IDLE = 0, WORKING, DONE, FAILED };
void ask(const char *prompt);
int state();
const char *reply();
const char *statusText();
bool haveConfig();
void clear();
} // namespace tdeckgemini

namespace
{
lv_obj_t *screen = nullptr;
lv_obj_t *askArea = nullptr;
lv_obj_t *answerLbl = nullptr;
lv_obj_t *statusLbl = nullptr;
lv_obj_t *askBtnLbl = nullptr;
lv_timer_t *poll = nullptr;
lv_timer_t *focusGuard = nullptr;
int lastState = -1;

lv_obj_t *makeButton(lv_obj_t *parent, const char *text, uint32_t colour, lv_event_cb_t cb, lv_obj_t **labelOut)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (labelOut)
        *labelOut = lbl;
    if (cb)
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void setStatus(const char *s, uint32_t colour)
{
    if (!statusLbl)
        return;
    lv_label_set_text(statusLbl, s);
    lv_obj_set_style_text_color(statusLbl, lv_color_hex(colour), LV_PART_MAIN);
}

void doAsk(lv_event_t *)
{
    if (!askArea)
        return;
    const char *q = lv_textarea_get_text(askArea);
    if (!q || !*q) {
        setStatus("Type a question first", 0xff9f0a);
        return;
    }
    if (!tdeckgemini::haveConfig()) {
        setStatus("No key - add one to /gemini.txt on the SD card", 0xff453a);
        return;
    }
    lv_label_set_text(answerLbl, "");
    setStatus("Asking... (this turns Bluetooth off)", 0x0a84ff);
    tdeckgemini::ask(q);
}

void onClear(lv_event_t *)
{
    tdeckgemini::clear();
    if (askArea)
        lv_textarea_set_text(askArea, "");
    if (answerLbl)
        lv_label_set_text(answerLbl, "");
    setStatus("", 0xffffff);
}

// Poll the state machine and repaint when it moves. Only on a CHANGE: the answer label is long
// and rewriting it every tick would make the list scroll jump under a reading thumb.
void tick(lv_timer_t *)
{
    const int st = tdeckgemini::state();
    if (st == lastState)
        return;
    lastState = st;
    switch (st) {
    case tdeckgemini::WORKING:
        lv_label_set_text(askBtnLbl, "...");
        setStatus(tdeckgemini::statusText(), 0x0a84ff);
        break;
    case tdeckgemini::DONE:
        lv_label_set_text(askBtnLbl, "Ask");
        lv_label_set_text(answerLbl, tdeckgemini::reply());
        setStatus("", 0xffffff);
        break;
    case tdeckgemini::FAILED:
        lv_label_set_text(askBtnLbl, "Ask");
        setStatus(tdeckgemini::statusText(), 0xff453a);
        break;
    default:
        lv_label_set_text(askBtnLbl, "Ask");
        break;
    }
}
} // namespace

extern "C" void gemini_open(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen); // the shared top bar, which also makes room for itself

        // The question. Two lines is enough for anything worth typing on a thumb keyboard, and
        // it leaves the rest of the screen for the answer, which is what you actually read.
        askArea = lv_textarea_create(screen);
        lv_obj_set_pos(askArea, 4, 28);
        lv_obj_set_size(askArea, 312, 52);
        lv_obj_set_style_bg_color(askArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(askArea, 0, LV_PART_MAIN);
        lv_textarea_set_max_length(askArea, 200);
        lv_textarea_set_placeholder_text(askArea, "Ask something...");
        // Solid non-blinking caret, for the same reason NotesApp uses one: a blinking thin line
        // only repaints when something else changes, so it looks like there is no cursor at all.
        lv_obj_set_style_bg_color(askArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(askArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(askArea, 0, LV_PART_CURSOR);
        if (lv_group_get_default())
            lv_group_add_obj(lv_group_get_default(), askArea);
        // Enter asks, rather than inserting a newline nobody wants in a one-line question.
        lv_obj_add_event_cb(askArea, [](lv_event_t *e) { doAsk(e); }, LV_EVENT_READY, NULL);

        lv_obj_t *askBtn = makeButton(screen, "Ask", 0x0a84ff, doAsk, &askBtnLbl);
        lv_obj_set_size(askBtn, 150, 34);
        lv_obj_set_pos(askBtn, 4, 86);
        lv_obj_t *clrBtn = makeButton(screen, "Clear", 0x3a3a3c, onClear, nullptr);
        lv_obj_set_size(clrBtn, 158, 34);
        lv_obj_set_pos(clrBtn, 158, 86);

        statusLbl = lv_label_create(screen);
        lv_obj_set_pos(statusLbl, 6, 124);
        lv_obj_set_width(statusLbl, 308);
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(statusLbl, "");

        // The answer, scrollable — Gemini is told to be brief but "brief" still overflows 100px.
        lv_obj_t *box = lv_obj_create(screen);
        lv_obj_set_pos(box, 2, 142);
        lv_obj_set_size(box, 316, 96);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x121214), LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(box, 6, LV_PART_MAIN);
        answerLbl = lv_label_create(box);
        lv_obj_set_width(answerLbl, 296);
        lv_label_set_long_mode(answerLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(answerLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(answerLbl, "");

        poll = lv_timer_create(tick, 250, NULL);
        // Keep the physical keyboard aimed at the question box. Same guard NotesApp needs:
        // anything that touches the focus group steals the keys otherwise.
        focusGuard = lv_timer_create(
            [](lv_timer_t *) {
                lv_group_t *g = lv_group_get_default();
                if (g && askArea && lv_group_get_focused(g) != askArea)
                    lv_group_focus_obj(askArea);
            },
            300, NULL);
    }

    lastState = -1; // force the next tick to repaint whatever state we are resuming into
    if (poll)
        lv_timer_resume(poll);
    if (focusGuard)
        lv_timer_resume(focusGuard);
    if (!tdeckgemini::haveConfig())
        setStatus("No key yet - put one in /gemini.txt on the SD card", 0xff9f0a);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
