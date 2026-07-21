// -----------------------------------------------------------------------------
// T-Deck launcher: lock-screen on/off (firmware side).
//
// The launcher always boots to a PIN pad (a code is always in effect — default 1234
// until the user picks their own). Some people don't want a lock at all, so this adds
// a persistent on/off flag the Settings screen can flip.
//
// It lives in NVS — internal flash, no SPI — deliberately, NOT in the config that
// nodeDB writes over the shared filesystem. A flash write to that filesystem from the
// UI (LVGL) task is exactly what froze the device when the time-zone dropdown first
// tried it (see TDeckTimeZone.cpp). NVS via Preferences has its own lock and is
// task-safe, so the switch handler can write this directly with no deferred plumbing.
//
// Default is ON: a device nobody has touched keeps locking exactly as it did before.
// -----------------------------------------------------------------------------
#include <Preferences.h>

static int s_lockEnabled = -1; // -1 = not read from NVS yet; 0 = off; 1 = on

extern "C" bool tdeck_lock_enabled(void)
{
    if (s_lockEnabled < 0) {
        Preferences p;
        if (p.begin("tdecklock", true)) { // read-only
            s_lockEnabled = p.getBool("en", true) ? 1 : 0;
            p.end();
        } else {
            s_lockEnabled = 1; // namespace not created yet -> lock ON by default
        }
    }
    return s_lockEnabled != 0;
}

extern "C" void tdeck_lock_set_enabled(bool en)
{
    s_lockEnabled = en ? 1 : 0;
    Preferences p;
    if (p.begin("tdecklock", false)) { // read-write
        p.putBool("en", en);
        p.end();
    }
}
