// -----------------------------------------------------------------------------
// TDeckRemote - drive the device over the USB cable.
//
// Jake, 2026-09-19: "can you remotely control the device? Navigate it for me,
// screen cap etc?"
//
// WHY THIS IS ON USB AND NOT WI-FI. The obvious build was a little web server, but
// bringing Wi-Fi up on this hardware drops the Bluetooth link to his phone until the
// next reboot, and it would put an unauthenticated listener on his home network. The
// USB cable is already plugged in whenever I am working, costs him nothing, and can
// only be driven by something physically attached to the device.
//
// HOW IT SHARES THE PORT WITH MESHTASTIC. The USB serial carries Meshtastic's protobuf
// API. Its framing search (StreamAPI::readStream) throws away every byte that is not
// the 0x94 start marker and restarts - so plain text is already being discarded. This
// hooks exactly those discarded bytes, which means the two protocols cannot collide:
// anything we accept was, by definition, not part of a protobuf frame.
//
// Commands are one line, starting with "@@" so a stray newline is never mistaken for
// one. Replies come back as "@@ok ..." or "@@err ...", so a script can parse them out
// of the ordinary log chatter.
//
//   @@ping            - is anyone home
//   @@info            - uptime, active screen, free internal heap
//   @@mem             - full heap picture: free, largest block, all-time low
//   @@tap <x> <y>     - touch the screen at that point
//   @@swipe x1 y1 x2 y2 - drag, for paging the launcher and moving sliders
//   @@key <code>      - inject an LVGL keycode (see lv_keys)
//   @@home            - the Home gesture (trackball double-click)
//   @@back            - the Back gesture
//   @@shot            - take a screenshot to internal flash
//   @@get             - stream the last capture back (RLE+base64, ~2s)
//
// THREADING: the bytes arrive on whichever task runs the serial API, so this file only
// ever parses and queues. Everything that touches LVGL is executed by the UI poll timer
// on the "tft" task - see tdeck_remote_take(). That is the same rule as the rest of the
// launcher bridges; see the notes in TDeckPop.cpp.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

static const int kMaxLine = 64;
static char s_line[kMaxLine];
static int s_len = 0;
static bool s_armed = false; // saw '@' '@' at the start of a line

// One pending command, parsed. The UI task picks it up.
static volatile int s_cmd = 0; // 0 none, 1 tap, 2 home, 3 back, 4 shot, 5 info, 6 ping
static volatile int s_x = 0, s_y = 0;
static volatile int s_x2 = 0, s_y2 = 0; // swipe end point

extern "C" void tdeck_shot_stream_begin(void); // src/TDeckScreenshot.cpp

// Fed one byte at a time from the protobuf framing search, for bytes it rejected.
extern "C" void tdeck_remote_feed(uint8_t c)
{
    if (c == '\r')
        return;
    if (c == '\n') {
        if (s_armed && s_len > 0) {
            s_line[s_len] = 0;
            // Parse here (cheap, no allocation); execute on the UI task.
            if (!strncmp(s_line, "ping", 4)) {
                s_cmd = 6;
            } else if (!strncmp(s_line, "info", 4)) {
                s_cmd = 5;
            } else if (!strncmp(s_line, "mem", 3)) {
                // Memory is answered straight here, not on the UI task: it reads heap
                // counters only, and the UI task is the one under pressure.
                LOG_INFO("@@ok mem internal free=%u largest=%u min=%u | psram free=%u largest=%u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            } else if (!strncmp(s_line, "shot", 4)) {
                s_cmd = 4;
            } else if (!strncmp(s_line, "get", 3)) {
                // Streams the last capture back over this same cable. Handled straight on
                // the main loop rather than the UI task - it only reads the PSRAM buffer,
                // and the UI task holds the SPI lock the radio needs.
                tdeck_shot_stream_begin();
            } else if (!strncmp(s_line, "home", 4)) {
                s_cmd = 2;
            } else if (!strncmp(s_line, "back", 4)) {
                s_cmd = 3;
            } else if (!strncmp(s_line, "swipe", 5)) {
                int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
                if (sscanf(s_line + 5, "%d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
                    s_x = x1; s_y = y1; s_x2 = x2; s_y2 = y2;
                    s_cmd = 7;
                } else {
                    LOG_INFO("@@err swipe needs x1 y1 x2 y2");
                }
            } else if (!strncmp(s_line, "key", 3)) {
                int k = -1;
                if (sscanf(s_line + 3, "%d", &k) == 1 && k > 0) {
                    s_x = k;
                    s_cmd = 8;
                } else {
                    LOG_INFO("@@err key needs a keycode");
                }
            } else if (!strncmp(s_line, "tap", 3)) {
                int x = -1, y = -1;
                if (sscanf(s_line + 3, "%d %d", &x, &y) == 2 && x >= 0 && x < 320 && y >= 0 && y < 240) {
                    s_x = x;
                    s_y = y;
                    s_cmd = 1;
                } else {
                    LOG_INFO("@@err tap needs x y within 0,0..319,239");
                }
            } else {
                LOG_INFO("@@err unknown command");
            }
        }
        s_len = 0;
        s_armed = false;
        return;
    }
    if (!s_armed) {
        // Waiting for the "@@" prefix. s_len counts the '@'s seen so far.
        if (c == '@') {
            if (++s_len >= 2) {
                s_armed = true;
                s_len = 0;
            }
        } else {
            s_len = 0;
        }
        return;
    }
    if (s_len < kMaxLine - 1)
        s_line[s_len++] = (char)c;
}

// Called by the UI poll timer on the tft task. Returns the pending command (and clears
// it), so that everything touching LVGL happens where LVGL lives.
extern "C" int tdeck_remote_take(int *x, int *y, int *x2, int *y2)
{
    const int c = s_cmd;
    if (!c)
        return 0;
    if (x)
        *x = s_x;
    if (y)
        *y = s_y;
    if (x2)
        *x2 = s_x2;
    if (y2)
        *y2 = s_y2;
    s_cmd = 0;
    return c;
}

// Replies go out as ordinary log lines with a @@ prefix, so they ride the existing
// serial output and a script can grep them out of the chatter.
extern "C" void tdeck_remote_reply(const char *what)
{
    LOG_INFO("@@ok %s", what ? what : "");
}
