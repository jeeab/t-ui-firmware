// -----------------------------------------------------------------------------
// T-Deck launcher: node list + favourites bridge (firmware side)
//
// Jake, 2026-09-10: "for the tdeck color, can we make a favorites app as well?
// which means we need a way to favorite them .. which means we need a separate
// node list app. trying to keep the meshtastic app modified the least possible
// so that updates are easier in the future."
//
// That constraint is the whole reason this file exists. The launcher's Nodes and
// Favorites apps are their own LVGL screens and touch NOTHING inside MUI's own
// node panels, so a future device-ui update merges cleanly. device-ui cannot
// include firmware headers (see TDeckGpsBridge.cpp), so everything it needs is
// exposed here as extern "C" free functions over plain scalars — no shared
// structs, nothing to keep in step across the boundary.
//
// The star itself is Meshtastic's own NodeInfo favourite bit, reached through
// nodeDB->isFavorite()/set_favorite(). Nothing is stored on our side: a node
// starred here is starred in the phone app too, and it survives a reboot,
// because it is the same flag the rest of the firmware already maintains.
//
// THREADING: all of these run on the LVGL/UI task and read the long-lived nodeDB
// owned by the mesh task. Reads are of small scalar fields (or copies made under
// NodeDB's own lock via copyNodePosition/copyNodeTelemetry), and a UI list that
// is one update stale is harmless. set_favorite() is a write, but it is the same
// call MUI's own node menu makes, so it is no less safe here than there.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "gps/RTC.h" // getValidTime() / RTCQualityFromNet
#include "mesh/NodeDB.h"
#include <cstring>

// Pending favourite toggle, handed from the UI thread to tdeck_nodes_service() on the firmware
// thread. Declared up here because tdeck_node_is_favorite() reads them before the setter appears.
static volatile uint32_t s_favPendingNum = 0;
static volatile bool s_favPendingOn = false;
static volatile bool s_favPending = false;


// Node numbers for the launcher's list, ready to draw: OURSELVES EXCLUDED, and
// ordered favourites-first, then most-recently-heard within each group. Sorting
// here rather than in the UI keeps the two apps identical by construction — the
// Favorites app is simply this list filtered to the starred ones.
// Returns how many were written (never more than maxN).
extern "C" int tdeck_nodes_list(uint32_t *out, int maxN)
{
    if (!out || maxN <= 0 || !nodeDB)
        return 0;
    const uint32_t me = nodeDB->getNodeNum();
    const size_t total = nodeDB->getNumMeshNodes();
    int n = 0;
    for (size_t i = 0; i < total && n < maxN; i++) {
        meshtastic_NodeInfoLite *e = nodeDB->getMeshNodeByIndex(i);
        if (!e || !e->num || e->num == me)
            continue;
        out[n++] = e->num;
    }
    // Insertion sort: n is small and this runs on a screen open, not per frame.
    for (int a = 1; a < n; a++) {
        const uint32_t key = out[a];
        const bool keyFav = nodeDB->isFavorite(key);
        meshtastic_NodeInfoLite *ke = nodeDB->getMeshNode(key);
        const uint32_t keyHeard = ke ? ke->last_heard : 0;
        int b = a - 1;
        while (b >= 0) {
            const bool bFav = nodeDB->isFavorite(out[b]);
            meshtastic_NodeInfoLite *be = nodeDB->getMeshNode(out[b]);
            const uint32_t bHeard = be ? be->last_heard : 0;
            const bool keyWins = (keyFav != bFav) ? keyFav : (keyHeard > bHeard);
            if (!keyWins)
                break;
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}

// Long name if the node has told us one, otherwise "!xxxxxxxx" from its number —
// never an empty row. Points at a rotating set of static buffers so several calls
// can be live in one draw without the caller having to own storage.
extern "C" const char *tdeck_node_name(uint32_t num)
{
    static char bufs[4][40];
    static int slot = 0;
    char *b = bufs[slot];
    slot = (slot + 1) & 3;
    meshtastic_NodeInfoLite *e = nodeDB ? nodeDB->getMeshNode(num) : nullptr;
    if (e && nodeInfoLiteHasUser(e) && e->long_name[0])
        snprintf(b, sizeof(bufs[0]), "%s", e->long_name);
    else
        snprintf(b, sizeof(bufs[0]), "!%08x", (unsigned)num);
    return b;
}

// Seconds since we last heard from this node; 0 = never heard / clock not set yet.
extern "C" uint32_t tdeck_node_age_secs(uint32_t num)
{
    meshtastic_NodeInfoLite *e = nodeDB ? nodeDB->getMeshNode(num) : nullptr;
    if (!e || !e->last_heard)
        return 0;
    const uint32_t now = getValidTime(RTCQualityFromNet);
    return (now > e->last_heard) ? (now - e->last_heard) : 0;
}

// Battery percent, or -1 when the node has not reported one. Meshtastic sends
// anything over 100 to mean "not on a battery" (USB/mains) — passed through as
// 101 so the UI can say "USB" rather than inventing a percentage.
extern "C" int tdeck_node_battery(uint32_t num)
{
    meshtastic_DeviceMetrics m;
    if (!nodeDB || !nodeDB->copyNodeTelemetry(num, m) || !m.has_battery_level)
        return -1;
    return (int)m.battery_level;
}

// Last known position, 1e-7 degrees. false = this node has never sent one.
extern "C" bool tdeck_node_position(uint32_t num, int32_t *latI, int32_t *lonI)
{
    meshtastic_PositionLite p;
    if (!nodeDB || !nodeDB->copyNodePosition(num, p))
        return false;
    if (!p.latitude_i && !p.longitude_i)
        return false;
    if (latI)
        *latI = p.latitude_i;
    if (lonI)
        *lonI = p.longitude_i;
    return true;
}

extern "C" bool tdeck_node_is_favorite(uint32_t num)
{
    // Answer optimistically for a toggle that has not been applied yet, so the star flips under the
    // finger instead of waiting a loop for the firmware thread to catch up.
    if (s_favPending && s_favPendingNum == num)
        return s_favPendingOn;
    return nodeDB && nodeDB->isFavorite(num);
}

// ⛔ NEVER TOUCH STORAGE FROM THE UI THREAD. Jake, twice: "froze when i added something to my
// favorites."
//
// NodeDB::set_favorite() is not the small flag write it looks like — it calls sortMeshDB() AND
// saveNodeDatabaseToDisk() internally, so one tap re-sorts the whole database and writes all 250
// nodes to flash. The first version of this then called saveToDisk() on top of that, writing them
// twice. Doing that from the LVGL task, while the mesh task owns the same storage, is the deadlock
// this project already has a rule about (the map pins defer their SD writes to a service tick for
// exactly this reason). The first freeze left a 0-byte nodes.proto.tmp and cost Jake 250 nodes.
//
// So the tap only RECORDS the request. tdeck_nodes_service(), called from the main loop on the
// firmware thread, is what actually touches nodeDB — the thread that owns it, where a flash write
// is ordinary.
extern "C" void tdeck_node_set_favorite(uint32_t num, bool on)
{
    s_favPendingNum = num;
    s_favPendingOn = on;
    s_favPending = true; // set LAST: the service tick reads this to decide the rest is valid
}

// Runs on the firmware thread from the main loop. Cheap: one bool test in the common case.
extern "C" void tdeck_nodes_service(void)
{
    if (!s_favPending)
        return;
    s_favPending = false;
    if (!nodeDB)
        return;
    const uint32_t num = s_favPendingNum;
    const bool on = s_favPendingOn;
    nodeDB->set_favorite(on, num); // this saves the database itself - do NOT save again
    LOG_INFO("[TUIFAV] 0x%08x %s", (unsigned)num, on ? "favourited" : "un-favourited");
}

// -----------------------------------------------------------------------------
// Channels — for the launcher's Channels app.
//
// ⚠️ MESHTASTIC DOES NOT TRACK CHANNEL MEMBERSHIP. A node never advertises which
// channels it is on, so "who is in this channel" is not a question the mesh can
// answer. What the device DOES know is the channel it last heard each node's
// NodeInfo on — NodeDB sets info->channel for exactly that reason ("the channel we
// need to use to reach this node"). So the app lists who we have HEARD on a
// channel, and says so in those words rather than implying a roster.
// -----------------------------------------------------------------------------
#include "mesh/Channels.h"

extern "C" int tdeck_channel_count(void)
{
    return (int)channels.getNumChannels();
}

// Name as configured. The primary comes back as its modem preset name (e.g. "LongFast")
// when it has no explicit name, which is what the phone app shows too.
extern "C" const char *tdeck_channel_name(int idx)
{
    if (idx < 0 || idx >= (int)channels.getNumChannels())
        return "";
    return channels.getName((ChannelIndex)idx);
}

// 0 = disabled (don't show it), 1 = primary, 2 = secondary.
extern "C" int tdeck_channel_role(int idx)
{
    if (idx < 0 || idx >= (int)channels.getNumChannels())
        return 0;
    return (int)channels.getByIndex((ChannelIndex)idx).role;
}

// position_precision for this channel: 32 = exact, lower = scrambled to a grid cell,
// 0 = positions not shared. This is the number that decides whether the map shows a
// node where it really is.
extern "C" int tdeck_channel_precision(int idx)
{
    if (idx < 0 || idx >= (int)channels.getNumChannels())
        return 0;
    return (int)channels.getByIndex((ChannelIndex)idx).settings.module_settings.position_precision;
}

// Nodes last heard on this channel, ourselves excluded, most-recently-heard first.
extern "C" int tdeck_channel_nodes(int chIdx, uint32_t *out, int maxN)
{
    if (!out || maxN <= 0 || !nodeDB)
        return 0;
    const uint32_t me = nodeDB->getNodeNum();
    const size_t total = nodeDB->getNumMeshNodes();
    int n = 0;
    for (size_t i = 0; i < total && n < maxN; i++) {
        meshtastic_NodeInfoLite *e = nodeDB->getMeshNodeByIndex(i);
        if (!e || !e->num || e->num == me)
            continue;
        if ((int)e->channel != chIdx)
            continue;
        out[n++] = e->num;
    }
    for (int a = 1; a < n; a++) {
        const uint32_t key = out[a];
        meshtastic_NodeInfoLite *ke = nodeDB->getMeshNode(key);
        const uint32_t kh = ke ? ke->last_heard : 0;
        int b = a - 1;
        while (b >= 0) {
            meshtastic_NodeInfoLite *be = nodeDB->getMeshNode(out[b]);
            if (!be || be->last_heard >= kh)
                break;
            out[b + 1] = out[b];
            b--;
        }
        out[b + 1] = key;
    }
    return n;
}
