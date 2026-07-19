// -----------------------------------------------------------------------------
// T-Deck launcher: time zone bridge (firmware side)
//
// Same pattern as TDeckGpsControl.cpp — device-ui can't include firmware headers,
// so these extern "C" functions let the Settings screen read and set the device's
// time zone.
//
// Why this exists: the device gets accurate UTC from the GPS satellites, but the
// stock build has no time zone unless one is configured, so main.cpp falls back to
// "GMT0". That's why the logs (and any clock we draw) read as UTC. Setting a zone
// here makes localtime() correct everywhere, including the launcher's clock.
//
// The strings are POSIX TZ rules and include the daylight-saving changeover dates,
// so the clock follows DST on its own with no further input.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "mesh/NodeDB.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Kept in step with the dropdown in the launcher's Settings screen (same order).
static const char *kTdeckZones[] = {
    "PST8PDT,M3.2.0,M11.1.0",     // Pacific
    "MST7MDT,M3.2.0,M11.1.0",     // Mountain
    "MST7",                       // Arizona (no DST)
    "CST6CDT,M3.2.0,M11.1.0",     // Central
    "EST5EDT,M3.2.0,M11.1.0",     // Eastern
    "AKST9AKDT,M3.2.0,M11.1.0",   // Alaska
    "HST10",                      // Hawaii
    "GMT0",                       // UTC
    "GMT0BST,M3.5.0/1,M10.5.0",   // UK
    "CET-1CEST,M3.5.0,M10.5.0/3", // Central Europe
};
static const int kTdeckZoneCount = sizeof(kTdeckZones) / sizeof(kTdeckZones[0]);

// Apply a zone by index. Takes effect immediately (no reboot) and persists.
extern "C" void tdeck_tz_set(int idx)
{
    if (idx < 0 || idx >= kTdeckZoneCount)
        return;
    strncpy(config.device.tzdef, kTdeckZones[idx], sizeof(config.device.tzdef) - 1);
    config.device.tzdef[sizeof(config.device.tzdef) - 1] = 0;
    setenv("TZ", config.device.tzdef, 1);
    tzset(); // localtime() is correct from here on, everywhere
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CONFIG);
    LOG_INFO("T-Deck time zone set to %s", config.device.tzdef);
}

// Which entry is currently in effect; -1 if it's something we don't have in the list
// (e.g. set from the phone app), so the UI can show it as unknown rather than lie.
extern "C" int tdeck_tz_get(void)
{
    for (int i = 0; i < kTdeckZoneCount; i++)
        if (strcmp(config.device.tzdef, kTdeckZones[i]) == 0)
            return i;
    return -1;
}
