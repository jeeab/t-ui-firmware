// -----------------------------------------------------------------------------
// T-UI Conversations — the people you are talking to, and the channels you listen to.
//
// Started life as Channels. Jake, 2026-09-18: "channeks app needs to be renamed, include
// covorsations too" / "should show all current conversations, not just channels". So the
// list is now People first (anyone this device has exchanged a DIRECT message with, newest
// first, with the last line of the conversation) and then Channels underneath, unchanged.
//
// Jake, 2026-09-10: "another cool app would be channels: So for me personally, it
// would show, long fast, and Howe group. on the custom channels (not long fast),
// there would be an icon ... that pulls up a simple list of whos in that custom
// channel."
//
// ⚠️ ONE HONEST LIMITATION, STATED ON THE SCREEN ITSELF. Meshtastic does not track
// channel membership — a node never advertises which channels it is on, so "who is
// in this channel" is not a question the mesh can answer. What the device DOES know
// is the channel it last heard each node's NodeInfo on, which NodeDB records
// precisely so it knows how to reach them. So the member list is headed "heard here"
// rather than pretending to be a roster, and a node that talks on two channels shows
// under whichever it was last heard on.
//
// Same arrangement as NodesApp: its own launcher screen, fed by extern "C" bridges,
// touching none of MUI's internals.
// -----------------------------------------------------------------------------
#include "lvgl.h"
#include <Arduino.h>
#include <cstdio>

extern "C" void channels_open(void);

// --- firmware bridge (src/TDeckNodesBridge.cpp) ---
extern "C" int tdeck_dm_conversations(uint32_t *out, uint32_t *lastWhen, int maxN);
extern "C" bool tdeck_dm_last_text(uint32_t nodeNum, char *out, int outN, bool *fromMe);
extern "C" int tdeck_channel_count(void);
extern "C" const char *tdeck_channel_name(int idx);
extern "C" int tdeck_channel_role(int idx);
extern "C" int tdeck_channel_precision(int idx);
extern "C" int tdeck_channel_nodes(int chIdx, uint32_t *out, int maxN);
extern "C" const char *tdeck_node_name(uint32_t num);
extern "C" uint32_t tdeck_node_age_secs(uint32_t num);

// --- MUI shim (TFTView_320x240.cpp) ---
extern "C" void tui_open_channel_chat(uint8_t ch);
extern "C" void tui_open_chat_with(uint32_t nodeNum);
extern "C" bool notif_unread_from(uint32_t nodeNum);
extern "C" void emoji_to_text(const char *in, char *out, size_t outN);

namespace
{
const int kMaxMembers = 60;

lv_obj_t *screen = nullptr;
lv_obj_t *listCont = nullptr;
lv_obj_t *titleLbl = nullptr;
lv_obj_t *noteLbl = nullptr;
lv_obj_t *prevScreen = nullptr;
int viewChannel = -1; // -1 = the main list; otherwise the members of that channel
uint32_t members[kMaxMembers];

// Jake, 2026-09-18: "channeks app needs to be renamed, include covorsations too" and
// "should show all current conversations, not just channels". So the app is Conversations
// now, and the list is both: the channels this device listens to, and every person it has
// actually exchanged a direct message with.
const int kMaxConvos = 24;
uint32_t convos[kMaxConvos];
uint32_t convoWhen[kMaxConvos];

void rebuild(void);

void ageText(uint32_t secs, char *out, size_t n)
{
    if (!secs) {
        out[0] = 0;
        return;
    }
    if (secs < 3600)
        snprintf(out, n, "seen %um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, n, "seen %uh", (unsigned)(secs / 3600));
    else
        snprintf(out, n, "seen %ud", (unsigned)(secs / 86400));
}

lv_obj_t *makeRow(int h)
{
    lv_obj_t *row = lv_obj_create(listCont);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *makeLabel(lv_obj_t *p, const char *txt, uint32_t colour, lv_align_t al, int dx, int dy)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_align(l, al, dx, dy);
    return l;
}

// Two small torso silhouettes — the same shape as the Nodes tile, shrunk to a button.
void peopleIcon(lv_obj_t *p)
{
    auto box = [&](int x, int y, int w, int h, uint32_t c, int r) {
        lv_obj_t *o = lv_obj_create(p);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_bg_color(o, lv_color_hex(c), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(o, r, LV_PART_MAIN);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    };
    box(6, 14, 10, 8, 0x6a6a70, 4);  // back shoulders
    box(8, 6, 7, 7, 0x6a6a70, 4);    // back head
    box(14, 15, 13, 9, 0x5ac8fa, 5); // front shoulders
    box(17, 5, 8, 8, 0x5ac8fa, 4);   // front head
}

// Jake: "in Channels ... can clicking the channel bring you to that conversation?"
// Deferred like every other handler here: this loads a different screen, which would tear down the
// widgets still dispatching the event.
void onOpenChat(lv_event_t *e)
{
    const int ch = (int)(intptr_t)lv_event_get_user_data(e);
    lv_async_call([](void *p) { tui_open_channel_chat((uint8_t)(intptr_t)p); }, (void *)(intptr_t)ch);
}

void onOpenDm(lv_event_t *e)
{
    const uint32_t num = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_async_call([](void *p) { tui_open_chat_with((uint32_t)(uintptr_t)p); }, (void *)(uintptr_t)num);
}

void onOpenMembers(lv_event_t *e)
{
    viewChannel = (int)(intptr_t)lv_event_get_user_data(e);
    rebuild();
}

void onBackToChannels(lv_event_t *)
{
    viewChannel = -1;
    rebuild();
}

void buildChannelList(void)
{
    lv_label_set_text(titleLbl, "Conversations");
    int shown = 0;

    // ---- the people you are actually talking to, first: that is what you open this for ----
    const int nConvo = tdeck_dm_conversations(convos, convoWhen, kMaxConvos);
    if (nConvo > 0) {
        lv_obj_t *hdr = lv_label_create(listCont);
        lv_label_set_text(hdr, "People");
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    }
    for (int i = 0; i < nConvo; i++) {
        shown++;
        lv_obj_t *row = makeRow(56);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onOpenDm, LV_EVENT_CLICKED, (void *)(uintptr_t)convos[i]);

        const bool unread = notif_unread_from(convos[i]);
        if (unread) {
            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 9, 9);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x30d158), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 9, 11);
        }
        makeLabel(row, tdeck_node_name(convos[i]), 0xffffff, LV_ALIGN_TOP_LEFT, unread ? 24 : 10, 6);

        // The last thing said, and who said it, so the row is worth reading at a glance.
        char last[80];
        bool mine = false;
        if (tdeck_dm_last_text(convos[i], last, sizeof(last), &mine)) {
            char shown[80];
            emoji_to_text(last, shown, sizeof(shown)); // same reason as the message bubbles
            char line[96];
            snprintf(line, sizeof(line), "%s%s", mine ? "You: " : "", shown);
            lv_obj_t *l = makeLabel(row, line, unread ? 0x30d158 : 0x8e8e93, LV_ALIGN_BOTTOM_LEFT, 10, -6);
            lv_obj_set_width(l, 240);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 10, -6); // re-align: setting a width moves it
        } else {
            makeLabel(row, "No messages yet", 0x8e8e93, LV_ALIGN_BOTTOM_LEFT, 10, -6);
        }

        char age[16];
        ageText(tdeck_node_age_secs(convos[i]), age, sizeof(age));
        if (age[0])
            makeLabel(row, age, 0x8e8e93, LV_ALIGN_TOP_RIGHT, -10, 6);
    }

    if (nConvo > 0) {
        lv_obj_t *hdr = lv_label_create(listCont);
        lv_label_set_text(hdr, "Channels");
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    }
    for (int i = 0; i < tdeck_channel_count(); i++) {
        const int role = tdeck_channel_role(i);
        if (role == 0)
            continue; // disabled slots are not channels
        shown++;
        lv_obj_t *row = makeRow(56);
        // The row itself opens the conversation. The people button is a child with its own handler,
        // so it takes its own taps and does not fall through to this.
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onOpenChat, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        const char *nm = tdeck_channel_name(i);
        makeLabel(row, (nm && nm[0]) ? nm : "(unnamed)", 0xffffff, LV_ALIGN_TOP_LEFT, 10, 6);

        // The precision is the interesting fact about a channel here: it is what decides
        // whether the map can show people where they actually are.
        const int prec = tdeck_channel_precision(i);
        char sub[64];
        if (role == 1)
            snprintf(sub, sizeof(sub), "Primary  -  %s", prec >= 32 ? "exact positions" : (prec > 0 ? "approx positions" : "no positions"));
        else
            snprintf(sub, sizeof(sub), "Secondary  -  %s", prec >= 32 ? "exact positions" : (prec > 0 ? "approx positions" : "no positions"));
        makeLabel(row, sub, 0x8e8e93, LV_ALIGN_BOTTOM_LEFT, 10, -6);

        // People button on the custom channels only, exactly as asked — the primary is
        // everyone by definition, so a list there says nothing.
        if (role != 1) {
            lv_obj_t *btn = lv_obj_create(row);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 44, 44);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -6, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(btn, onOpenMembers, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            peopleIcon(btn);
        }
    }
    if (!shown)
        lv_label_set_text(noteLbl, "No conversations yet.\nChannels appear here once configured,\nand people once you have messaged.");
    else
        lv_label_set_text(noteLbl, "");
}

void buildMemberList(void)
{
    const char *nm = tdeck_channel_name(viewChannel);
    lv_label_set_text(titleLbl, (nm && nm[0]) ? nm : "Channel");

    lv_obj_t *back = makeRow(36);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, onBackToChannels, LV_EVENT_CLICKED, nullptr);
    makeLabel(back, LV_SYMBOL_LEFT "  All conversations", 0x0a84ff, LV_ALIGN_LEFT_MID, 10, 0);

    const int n = tdeck_channel_nodes(viewChannel, members, kMaxMembers);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = makeRow(40);
        makeLabel(row, tdeck_node_name(members[i]), 0xffffff, LV_ALIGN_LEFT_MID, 10, 0);
        char age[16];
        ageText(tdeck_node_age_secs(members[i]), age, sizeof(age));
        if (age[0])
            makeLabel(row, age, 0x8e8e93, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    // Said plainly, because the alternative is a list that quietly lies about what it is.
    if (n == 0)
        lv_label_set_text(noteLbl, "Nobody heard on this channel yet.\nNodes appear here once their\ninfo arrives over it.");
    else
        lv_label_set_text(noteLbl, "");
}

void rebuild(void)
{
    lv_obj_clean(listCont);
    if (viewChannel < 0)
        buildChannelList();
    else
        buildMemberList();
    if (lv_label_get_text(noteLbl) && lv_label_get_text(noteLbl)[0])
        lv_obj_clear_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
}

void buildScreen(void)
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_obj_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 56, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 6, LV_PART_MAIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        back,
        [](lv_event_t *) {
            if (prevScreen)
                lv_screen_load_anim(prevScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_center(bl);

    titleLbl = lv_label_create(screen);
    lv_label_set_text(titleLbl, "Channels");
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_align(titleLbl, LV_ALIGN_TOP_MID, 0, 9);

    listCont = lv_obj_create(screen);
    lv_obj_remove_style_all(listCont);
    lv_obj_set_size(listCont, 312, 198);
    lv_obj_align(listCont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_flex_flow(listCont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listCont, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listCont, LV_DIR_VER);

    noteLbl = lv_label_create(screen);
    lv_label_set_text(noteLbl, "");
    lv_obj_set_style_text_color(noteLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_set_style_text_align(noteLbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(noteLbl, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(noteLbl, LV_OBJ_FLAG_HIDDEN);
}
} // namespace

extern "C" void channels_open(void)
{
    if (!screen)
        buildScreen();
    lv_obj_t *active = lv_screen_active();
    if (active != screen)
        prevScreen = active;
    viewChannel = -1; // always open on the channel list, never mid-drill-down
    rebuild();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
