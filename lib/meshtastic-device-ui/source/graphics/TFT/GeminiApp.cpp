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
void release();
} // namespace tdeckgemini

namespace
{
lv_obj_t *screen = nullptr;
lv_obj_t *askArea = nullptr;
// True while the box holds a question that has already been sent: drawn grey, and
// wiped by the first keystroke of the next one.
bool s_sentText = false;
lv_obj_t *helpBox = nullptr; // "how do I get a key" overlay, behind the ? in the corner
size_t s_sentLen = 0; // how long the sent question was, so the next keystroke can drop exactly it
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

    // ⭐ THE SENT QUESTION GOES GREY AND GETS OUT OF THE WAY. Jake: "when I type and send a
    // message, the message retains there. Instead can me last sent message be grayed out in the
    // type box, and automatically be overwritten when I start typing again?"
    //
    // Leaving it black and editable reads as "this has not been sent yet", and the next question
    // has to be deleted character by character first. Grey says "this one is gone", and the first
    // keystroke clears it - see the LV_EVENT_VALUE_CHANGED handler where the textarea is built.
    s_sentText = true;
    s_sentLen = strlen(q);
    lv_obj_set_style_text_color(askArea, lv_color_hex(0x8e8e93), LV_PART_MAIN);
}

void onClear(lv_event_t *)
{
    tdeckgemini::clear();
    if (askArea) {
        lv_textarea_set_text(askArea, "");
        s_sentText = false;
        lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    }
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

// ⭐ GIVE THE MEMORY BACK WHEN THE APP IS NOT IN USE. Jake: "preferably Gemini closes and
// clears ram when not running". The textarea, buttons, scrollable answer box and two timers are
// INTERNAL RAM - the scarcest thing here - and were being held for the rest of the session after
// a single visit. gemini_open() already rebuilds everything from scratch, so there is nothing to
// preserve.
//
// ⛔ NOT WHILE A REQUEST IS IN FLIGHT. Deleting the screen mid-request would destroy the label
// the reply is about to be written into. Five seconds of grace as well, so paging past Gemini
// on the launcher does not tear it down and immediately rebuild it.
extern "C" void gemini_idle_check(void)
{
    static uint32_t idleSince = 0;
    if (!screen)
        return;
    if (lv_screen_active() == screen || tdeckgemini::state() == tdeckgemini::WORKING) {
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
    if (poll) {
        lv_timer_delete(poll);
        poll = nullptr;
    }
    if (focusGuard) {
        lv_timer_delete(focusGuard);
        focusGuard = nullptr;
    }
    lv_obj_delete(screen);
    screen = nullptr;
    askArea = answerLbl = statusLbl = askBtnLbl = nullptr;
    helpBox = nullptr;
    lastState = -1;
    tdeckgemini::release();
}

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
        // ⛔ AND DELETE THE ANIMATION THAT ALREADY EXISTS. lv_textarea_create() starts a
        // blinking cursor during construction using the DEFAULT time, so setting the duration to
        // zero above does not stop it - start_cursor_blink only re-reads that on focus, or on a
        // style change delivered to the label child. An active LVGL animation then forces a
        // refresh EVERY FRAME: measured 17-18 fps and 124-280 KB/s pushed while sitting idle,
        // against 2 fps once it is gone. Any later focus re-runs the check, finds the zero and
        // deletes it itself, so this one call is all that is needed.
        lv_anim_delete(askArea, nullptr);
        if (lv_group_get_default())
            lv_group_add_obj(lv_group_get_default(), askArea);
        // Enter asks, rather than inserting a newline nobody wants in a one-line question.
        lv_obj_add_event_cb(askArea, [](lv_event_t *e) { doAsk(e); }, LV_EVENT_READY, NULL);

        // The first keystroke after sending throws the old question away rather than appending to
        // it. Checked on VALUE_CHANGED because that is the only event that fires for a character
        // arriving from the keyboard, the trackball or a paste alike.
        lv_obj_add_event_cb(
            askArea,
            [](lv_event_t *) {
                if (!s_sentText || !askArea)
                    return;
                s_sentText = false;
                lv_obj_set_style_text_color(askArea, lv_color_hex(0xffffff), LV_PART_MAIN);
                // Keep only what was just typed: everything before it belonged to the old
                // question. One character in practice, but a paste is handled the same way.
                const char *t = lv_textarea_get_text(askArea);
                const size_t keep = t ? strlen(t) : 0;
                if (keep > s_sentLen && s_sentLen > 0) {
                    char tail[220];
                    snprintf(tail, sizeof(tail), "%s", t + s_sentLen);
                    lv_textarea_set_text(askArea, tail);
                }
                s_sentLen = 0;
            },
            LV_EVENT_VALUE_CHANGED, NULL);

        // ⭐ SCROLLABLE, because 200 characters do not fit in 52 pixels. Jake: "can that typing
        // box be scrollable?" - it could not; a long question simply ran out of sight with no way
        // to get back to it.
        lv_obj_set_scrollbar_mode(askArea, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(askArea, LV_DIR_VER);
        lv_textarea_set_one_line(askArea, false);

        // ⭐ "HOW DO I GET A KEY", tucked in the corner. Jake: "can you have a how to button
        // tucked in the corner saying how to get the api". Without it the only thing a new user
        // sees is "No key - add one to /gemini.txt on the SD card", which says WHERE to put a
        // thing it never tells them how to obtain. Same placement and styling as Mail's info
        // button, because two apps explaining a credential differently is one thing to learn
        // twice.
        {
            lv_obj_t *hb = makeButton(screen, "?", 0x2c2c2e, [](lv_event_t *) {
                if (helpBox) {
                    lv_obj_clear_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(helpBox);
                }
            }, nullptr);
            lv_obj_set_size(hb, 28, 28);
            lv_obj_set_pos(hb, 288, 0);
        }

        helpBox = lv_obj_create(screen);
        lv_obj_set_pos(helpBox, 0, 0);
        lv_obj_set_size(helpBox, 320, 218);
        lv_obj_set_style_bg_color(helpBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_border_width(helpBox, 0, LV_PART_MAIN);
        lv_obj_add_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
        {
            lv_obj_t *h = lv_label_create(helpBox);
            lv_label_set_text(h, "Getting a Gemini key");
            lv_obj_set_style_text_color(h, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_obj_set_pos(h, 4, 2);

            lv_obj_t *b = lv_label_create(helpBox);
            lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(b, 296);      // ⛔ WRAP NEEDS AN EXPLICIT WIDTH AND HEIGHT or it
            lv_obj_set_height(b, 150);     //    draws straight over the button below it
            lv_obj_set_pos(b, 4, 24);
            lv_label_set_text(b, "1. On a computer: aistudio.google.com\n"
                                 "2. Sign in, then \"Get API key\"\n"
                                 "3. Create a key and copy it\n"
                                 "4. On the SD card, make a file\n"
                                 "    called gemini.txt\n"
                                 "5. Put one line in it:\n"
                                 "    key=YOUR_KEY\n"
                                 "\n"
                                 "The key stays on the card - never\n"
                                 "built into the firmware, so the\n"
                                 "installer is safe to share.");
            lv_obj_set_style_text_color(b, lv_color_hex(0xc7c7cc), LV_PART_MAIN);

            lv_obj_t *ok = makeButton(helpBox, "Close", 0x3a3a3c, [](lv_event_t *) {
                if (helpBox)
                    lv_obj_add_flag(helpBox, LV_OBJ_FLAG_HIDDEN);
            }, nullptr);
            lv_obj_set_size(ok, 100, 30);
            lv_obj_set_pos(ok, 108, 182);
        }

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
