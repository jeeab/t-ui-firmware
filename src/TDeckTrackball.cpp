// -----------------------------------------------------------------------------
// T-Deck launcher: optional trackball navigation (firmware side).
//
// Rolling the trackball does nothing in T-UI. That is deliberate — everything is
// selected by touch, and the trackball's only job is the double-click that goes
// Home and wakes/locks the screen. It was never written down anywhere, though, so
// from the outside it is indistinguishable from a broken trackball: a T-Deck Plus
// owner opened an issue reporting exactly that (t-ui-firmware#1), with a working
// touchscreen and keyboard and a trackball that "does nothing in any direction".
//
// So the behaviour becomes a choice rather than a silent decision. When this is on,
// a roll emits the ordinary LVGL arrow keys again, which is what moved the focus
// highlight and flipped launcher pages before.
//
// DEFAULT OFF, for the same reason as the keyboard light: every device already in
// the world behaves the inert way, and a trackball that suddenly moves the focus
// when brushed is a surprise, not an upgrade. Opt in from Settings. Flag lives in
// NVS — see TDeckLockControl.cpp and TDeckKeyboardLight.cpp for the same pattern.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <Preferences.h>

static int s_tbNav = -1; // -1 = not read from NVS yet; 0 = off; 1 = on

extern "C" bool tdeck_trackball_nav_enabled(void)
{
    if (s_tbNav < 0) {
        Preferences p;
        if (p.begin("tdecktb", true)) { // read-only
            s_tbNav = p.getBool("nav", false) ? 1 : 0;
            p.end();
        } else {
            s_tbNav = 0; // namespace not created yet -> inert, exactly as before
        }
    }
    return s_tbNav != 0;
}

extern "C" void tdeck_trackball_nav_set_enabled(bool en)
{
    s_tbNav = en ? 1 : 0;
    Preferences p;
    if (p.begin("tdecktb", false)) { // read-write
        p.putBool("nav", en);
        p.end();
    }
    LOG_INFO("trackball nav: %s", en ? "on" : "off");
}

// -----------------------------------------------------------------------------
// Trackball click = select (separate switch).
//
// Kept apart from the roll setting on purpose: they answer different questions.
// Rolling is "can the trackball move the highlight"; clicking is "does a press
// choose the highlighted thing". Somebody may want one without the other.
//
// HOLDING the trackball goes Home, and that is NOT a setting - it is on always,
// for everybody, in both modes. It costs nothing (a hold is unambiguous the moment
// the button comes up) and it means there is always one gesture that gets you out,
// however the two switches below happen to be set.
// -----------------------------------------------------------------------------
static int s_tbClick = -1;

extern "C" bool tdeck_trackball_click_enabled(void)
{
    if (s_tbClick < 0) {
        Preferences p;
        if (p.begin("tdecktb", true)) {
            s_tbClick = p.getBool("click", false) ? 1 : 0;
            p.end();
        } else {
            s_tbClick = 0; // untouched device -> double-click keeps its old meaning
        }
    }
    return s_tbClick != 0;
}

extern "C" void tdeck_trackball_click_set_enabled(bool en)
{
    s_tbClick = en ? 1 : 0;
    Preferences p;
    if (p.begin("tdecktb", false)) {
        p.putBool("click", en);
        p.end();
    }
    LOG_INFO("trackball click-select: %s", en ? "on" : "off");
}
