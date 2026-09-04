// -----------------------------------------------------------------------------
// T-Deck launcher: slow the processor down while the screen is dark.
//
// The device already drops to 80 MHz for normal running (setCPUFast(false) in
// main.cpp). While the screen is off and locked, though, there is nothing to
// draw and nobody waiting on the UI, so it can go slower still - 40 MHz, which
// is the speed the T-Deck Max runs at all the time. On the ESP32-S3 the core
// draws roughly linearly with clock in this range, so this is a real saving on
// a device that spends most of its life in a pocket.
//
// TWO HARD GUARDS, because getting this wrong is worse than the battery it saves:
//
//   Wi-Fi  - the Espressif framework is unstable below 240 MHz with Wi-Fi up;
//            sleep.cpp already forces 240 for exactly this reason. If Wi-Fi is
//            connected we do not touch the clock at all.
//   Bluetooth - BLE timing is tight and this is not something I can test from
//            here, so the same rule applies: radio up, clock untouched.
//
// So the saving lands when both radios are quiet, which is the common case for a
// device sitting locked in a pack, and precisely the case where it matters most.
//
// DEFAULT OFF. It is a real behaviour change on a device I cannot test, and the
// consequences of a wrong clock show up while the device is locked - i.e. exactly
// when you are not looking at it. Opt in from Settings.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "mesh/NodeDB.h" // for `config` - the Bluetooth guard below reads config.bluetooth.enabled
#include <Preferences.h>

#if defined(ARCH_ESP32)
#include <esp32-hal-cpu.h>
#endif

extern "C" bool tdeck_wifi_connected(void); // TDeckWifi.cpp

static const uint32_t kIdleMhz = 40;  // locked and dark
static const uint32_t kAwakeMhz = 80; // the firmware's normal running speed

static int s_enabled = -1;   // -1 = not read from NVS yet
static bool s_throttled = false;

extern "C" bool tdeck_powersave_enabled(void)
{
    if (s_enabled < 0) {
        Preferences p;
        if (p.begin("tdeckps", true)) {
            s_enabled = p.getBool("en", false) ? 1 : 0;
            p.end();
        } else {
            s_enabled = 0;
        }
    }
    return s_enabled != 0;
}

extern "C" void tdeck_powersave_set_enabled(bool en)
{
    s_enabled = en ? 1 : 0;
    Preferences p;
    if (p.begin("tdeckps", false)) {
        p.putBool("en", en);
        p.end();
    }
    LOG_INFO("powersave: %s", en ? "on" : "off");
}

// Called whenever the screen goes dark or comes back. Safe to call repeatedly:
// it only acts when the state actually changes.
extern "C" void tdeck_powersave_dark(bool dark)
{
#if defined(ARCH_ESP32)
    if (!tdeck_powersave_enabled()) {
        if (s_throttled) { // switched off while slowed down - put it back
            setCpuFrequencyMhz(kAwakeMhz);
            s_throttled = false;
        }
        return;
    }

    // Waking always restores the clock FIRST, before anything tries to draw.
    if (!dark) {
        if (s_throttled) {
            setCpuFrequencyMhz(kAwakeMhz);
            s_throttled = false;
            LOG_INFO("powersave: %u MHz (awake)", (unsigned)kAwakeMhz);
        }
        return;
    }

    if (s_throttled)
        return;

    // Radio up: leave the clock alone entirely. Battery is not worth a dropped
    // connection or a wedged radio.
    if (tdeck_wifi_connected())
        return;
#if defined(HAS_BLUETOOTH) || defined(USE_NIMBLE)
    if (config.bluetooth.enabled)
        return;
#endif

    setCpuFrequencyMhz(kIdleMhz);
    s_throttled = true;
    LOG_INFO("powersave: %u MHz (screen dark)", (unsigned)kIdleMhz);
#else
    (void)dark;
#endif
}
