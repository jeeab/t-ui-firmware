// -----------------------------------------------------------------------------
// T-Deck launcher: GPS on/off control bridge (firmware side)
//
// Companion to TDeckGpsBridge.cpp (which only *reads* GPS status). This one lets
// the device-ui settings screen turn the GPS on or off and — when on — keep it
// searching continuously, applied LIVE with no reboot.
//
// Why a bridge + deferred service (same story as TDeckMeshSwitch.cpp): the LVGL
// UI runs on its own FreeRTOS task and must not drive the GPS driver directly (it
// sends UART commands and shares state with the GPS OSThread). So the UI-facing
// setter only records intent; tdeck_gps_control_service(), called from the main
// loop(), applies it on the same cooperative thread the GPS OSThread and
// AdminModule use — exactly where gps->enable()/disable() are already known-safe.
//
// "Continuous" = a gps_update_interval of 10s. At or below
// GPS_UPDATE_ALWAYS_ON_THRESHOLD_MS the GPS driver never powers the chip down
// between fixes (see GPS::down()), so it is always looking for satellites. The
// stock default is 120s, which is why the GPS otherwise sleeps in ~2-minute
// stretches and appears to "not look" until something happens to catch it awake.
// Longer intervals nap the chip between checks and save battery.
// -----------------------------------------------------------------------------
#if !MESHTASTIC_EXCLUDE_GPS
#include "gps/GPS.h"
#include "mesh/NodeDB.h"

static volatile bool s_tdeckGpsPending = false;          // an on/off change is waiting
static volatile bool s_tdeckGpsWantOn = true;            // desired state (written by the UI task)
static volatile uint32_t s_tdeckGpsPendingInterval = 0;  // desired check interval, seconds (0 = no change)

// Called from the UI (LVGL) task. Only records intent — never touches the GPS here.
extern "C" void tdeck_gps_set_enabled(bool on)
{
    s_tdeckGpsWantOn = on;
    s_tdeckGpsPending = true;
}

// How often the GPS looks for a fix, in seconds. <= 10 keeps the chip always on
// (continuous); longer intervals let it nap between checks to save battery.
extern "C" void tdeck_gps_set_interval(uint32_t secs)
{
    if (secs)
        s_tdeckGpsPendingInterval = secs;
}

// Live truth: is the GPS currently configured on?
extern "C" bool tdeck_gps_get_enabled(void)
{
    return config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED;
}

// Called from the main Meshtastic loop() — the safe GPS context. Applies pending
// requests; retries next loop if the GPS driver isn't up yet. Cheap when idle.
extern "C" void tdeck_gps_control_service(void)
{
    if (!s_tdeckGpsPending && !s_tdeckGpsPendingInterval)
        return;

    if (!gps)
        return; // GPS driver not ready yet — keep the request pending, retry next loop

    // Apply a new check interval first so an accompanying "on" enables with it.
    uint32_t interval = s_tdeckGpsPendingInterval;
    if (interval) {
        config.position.gps_update_interval = interval;
        s_tdeckGpsPendingInterval = 0;
        // Already on and staying on? Re-enable so the driver reschedules with the
        // new interval now instead of after its current (possibly long) nap.
        if (!s_tdeckGpsPending && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED)
            gps->enable();
    }

    if (s_tdeckGpsPending) {
        if (s_tdeckGpsWantOn) {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
            gps->enable(); // resets scheduling and starts a search immediately
        } else {
            config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
            gps->disable();
        }
        s_tdeckGpsPending = false;
    }

    // Persist so the choice survives a reboot. Unlike the admin config path, this
    // does NOT force a device restart.
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
}

#else // GPS excluded from this build — provide no-op stubs so callers still link.
extern "C" void tdeck_gps_set_enabled(bool) {}
extern "C" void tdeck_gps_set_interval(uint32_t) {}
extern "C" bool tdeck_gps_get_enabled(void)
{
    return false;
}
extern "C" void tdeck_gps_control_service(void) {}
#endif // !MESHTASTIC_EXCLUDE_GPS
