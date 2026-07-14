// -----------------------------------------------------------------------------
// T-Deck launcher: GPS status bridge (firmware side)
//
// Same idea as TDeckMeshSwitch.cpp: device-ui can't include firmware headers,
// so these extern "C" free functions expose the GPS driver's live state to the
// UI — satellites in view and lock/fix straight from gpsStatus, available while
// the GPS is still acquiring, before any position packet reaches the UI feed.
//
// THREADING: these are pure reads of small scalar fields on the long-lived
// gpsStatus singleton (updated by the GPS OSThread). Word-sized reads on the
// ESP32-S3 don't tear, and a stale-by-one-update value is harmless for a UI
// readout, so no locking is needed.
// -----------------------------------------------------------------------------
#include "GPSStatus.h"

extern "C" uint32_t tdeck_gps_num_sats(void)
{
    return gpsStatus ? gpsStatus->getNumSatellites() : 0;
}

extern "C" bool tdeck_gps_has_lock(void)
{
    return gpsStatus && gpsStatus->getHasLock();
}

// Current fix (1e-7 degrees, same scale the UI already uses). Returns false
// until there's a usable position. getLatitude()/getLongitude() also cover the
// fixed-position config case, where hasLock may stay false.
extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon)
{
    if (!gpsStatus)
        return false;
    if (!gpsStatus->getHasLock() && !config.position.fixed_position)
        return false;
    int32_t la = gpsStatus->getLatitude();
    int32_t lo = gpsStatus->getLongitude();
    if (la == 0 && lo == 0)
        return false;
    *lat = la;
    *lon = lo;
    return true;
}
