// -----------------------------------------------------------------------------
// TDeckMemInfo — tiny firmware-side bridge so the device-ui layer can read live
// RAM figures. The UI library can't include firmware src/ headers, so it links
// to these free functions across the boundary (same pattern as TDeckMeshSwitch
// and TDeckGpsBridge). Pure scalar reads — no locking needed.
//
// Used by the launcher's on-screen memory readout AND the crash investigation:
//   * current + lowest-since-boot free heap (the small fast RAM)
//   * current + lowest-since-boot free PSRAM (the big slow RAM the maps decode
//     tiles into — invisible before, and the prime crash suspect)
//   * WHY the device last restarted (esp_reset_reason) — this is what finally
//     tells RAM-vs-CPU apart: a real code fault (CRASH) vs a hung processor the
//     safety timer rebooted (FROZE) vs a power dip (BROWNOUT) leave different
//     fingerprints.
//   * the worst-case memory lows from the PREVIOUS session, persisted to NVS so
//     they survive the crash+reboot (getMinFree* alone resets every boot).
// -----------------------------------------------------------------------------
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

extern "C" uint32_t tdeck_free_heap(void)
{
    return ESP.getFreeHeap();
}

// Lowest free-heap watermark since boot — the number that actually predicts a
// crash. Survives navigating around (only a reboot resets it).
extern "C" uint32_t tdeck_min_free_heap(void)
{
    return ESP.getMinFreeHeap();
}

extern "C" uint32_t tdeck_free_psram(void)
{
    return ESP.getFreePsram();
}

// Lowest free-PSRAM watermark since boot — the big-slow-RAM equivalent, and the
// number we were flying blind on before.
extern "C" uint32_t tdeck_min_free_psram(void)
{
    return ESP.getMinFreePsram();
}

// ---- restart reason + cross-reboot low-water persistence --------------------

static bool s_diagInit = false;
static int s_prevReason = ESP_RST_UNKNOWN;
static uint32_t s_prevPsramLow = 0; // last session's worst PSRAM free
static uint32_t s_prevHeapLow = 0;  // last session's worst heap free
static uint32_t s_savedPsramLow = 0xFFFFFFFF;
static uint32_t s_savedHeapLow = 0xFFFFFFFF;

// Capture-once at boot: read WHY we restarted and last session's saved lows,
// then start recording this session's lows fresh. Safe to call more than once
// (guarded) — call it before wiring up the readout.
extern "C" void tdeck_diag_boot(void)
{
    if (s_diagInit)
        return;
    s_diagInit = true;

    s_prevReason = (int)esp_reset_reason();

    Preferences p;
    if (p.begin("tdeckdiag", true)) { // read-only
        s_prevPsramLow = p.getULong("psl", 0);
        s_prevHeapLow = p.getULong("hpl", 0);
        p.end();
    }

    // Seed this session's saved lows with the current (high) free values, and
    // write them so a crash before the first new-low still leaves a sane record.
    s_savedPsramLow = ESP.getFreePsram();
    s_savedHeapLow = ESP.getFreeHeap();
    if (p.begin("tdeckdiag", false)) {
        p.putULong("psl", s_savedPsramLow);
        p.putULong("hpl", s_savedHeapLow);
        p.end();
    }
}

// Call periodically (from the 1s UI timer). Persists a new low-water mark to
// NVS only when it drops meaningfully (>8k) below what's already saved — so the
// worst PSRAM/heap free right before a crash survives the reboot, without
// hammering the flash on every tick.
extern "C" void tdeck_diag_tick(void)
{
    if (!s_diagInit)
        return;
    uint32_t ps = ESP.getFreePsram();
    uint32_t hp = ESP.getFreeHeap();
    bool changed = false;
    if (ps + 8192 < s_savedPsramLow) {
        s_savedPsramLow = ps;
        changed = true;
    }
    if (hp + 8192 < s_savedHeapLow) {
        s_savedHeapLow = hp;
        changed = true;
    }
    if (changed) {
        Preferences p;
        if (p.begin("tdeckdiag", false)) {
            p.putULong("psl", s_savedPsramLow);
            p.putULong("hpl", s_savedHeapLow);
            p.end();
        }
    }
}

extern "C" int tdeck_prev_reason(void)
{
    return s_prevReason;
}

// Short human label for the previous restart cause.
extern "C" const char *tdeck_prev_reason_str(void)
{
    switch (s_prevReason) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_SW:
        return "restart";
    case ESP_RST_PANIC:
        return "CRASH"; // code fault / bad memory access
    case ESP_RST_INT_WDT:
        return "FROZE"; // interrupt watchdog — cpu hung
    case ESP_RST_TASK_WDT:
        return "FROZE (task)"; // task watchdog — a task hogged the cpu
    case ESP_RST_WDT:
        return "FROZE (wdt)";
    case ESP_RST_BROWNOUT:
        return "power dip"; // supply voltage sagged
    case ESP_RST_DEEPSLEEP:
        return "wake";
    case ESP_RST_EXT:
        return "ext reset";
    default:
        return "unknown";
    }
}

// True when the previous restart looks like a genuine fault worth logging/alerting.
extern "C" bool tdeck_prev_reason_bad(void)
{
    switch (s_prevReason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

extern "C" uint32_t tdeck_prev_psram_low(void)
{
    return s_prevPsramLow;
}

extern "C" uint32_t tdeck_prev_heap_low(void)
{
    return s_prevHeapLow;
}
