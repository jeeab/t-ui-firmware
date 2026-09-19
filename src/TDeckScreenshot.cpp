// -----------------------------------------------------------------------------
// TDeckScreenshot - save what is on the screen to the SD card.
//
// Jake, 2026-09-18: "debug: screen shot thar we can both trigger. saves to sd".
// The point is that he can show me exactly what he is looking at, instead of
// describing it, and that a bug report can carry a picture.
//
// HOW IT CAPTURES. Not by reading pixels back off the panel: the ST7789 shares its
// SPI bus with the SD card and the radio, readback there is unreliable, and holding
// that bus is what has been stalling the radio and rebooting the device. Instead the
// LVGL flush callback - which already has the finished pixels in its hand on their way
// to the glass - copies them into a PSRAM buffer. That also means the shot contains
// EXACTLY what is displayed, overlays and pop-ups included, which a per-screen snapshot
// would miss.
//
// HOW IT WRITES. Not from the flush callback. That runs on the "tft" task, mid-refresh, and
// tftSetup.cpp holds the SPI LOCK across that whole task, so doing anything slow there holds
// the bus the radio is waiting for. The callback only fills the buffer and sets a flag;
// tdeck_shot_service() on the MAIN loop does the writing.
//
// WHERE IT WRITES - Jake, 2026-09-19: "could we have the screenshot save to the internal
// memory instead so i dont need to plug int he card everytime?"
//
// So: INTERNAL FLASH (LittleFS / FSCom), not the SD card. Two happy consequences beyond the
// obvious one. It works with no card in the device at all. And internal flash is NOT on the
// shared SPI bus that the display, the card and the radio fight over - it has its own flash
// controller - so this no longer needs spiLock and can never stall the radio.
//
// The partition is 0x360000 (3.5 MB), shared with Meshtastic's own /prefs (config plus a node
// database that runs to six figures of bytes with 248 nodes). A 24-bit 320x240 BMP is 230 KB,
// so this keeps the newest kKeep and deletes the rest, and refuses outright if free space
// looks tight. Losing somebody's node database to make room for a screenshot would be a
// genuinely bad trade.
//
// Getting them off: `esptool read_flash 0xc90000 0x360000 fs.bin`, then read the image with
// littlefs-python (already installed in the penv). No Wi-Fi, no card, no new protocol.
//
// 24-bit BMP is bigger than it needs to be, but every computer and phone opens it without
// thinking, which is the whole point of a screenshot you intend to send to somebody.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <Arduino.h> // millis()
#include <cstdio>
#include <cstring>

#if !defined(ARCH_PORTDUINO)
#include "FSCommon.h" // FSCom = LittleFS on internal flash
#include <esp_heap_caps.h>
#define SHOT_HAVE_FS 1
#else
#define SHOT_HAVE_FS 0
#endif

// Keep this many. Four 230 KB screenshots is under a megabyte of a 3.5 MB partition, which
// leaves plenty for the node database, and four is more than any bug report needs.
static const int kKeep = 4;
// Refuse below this much free space, so a screenshot can never be the thing that stops
// Meshtastic saving its nodes.
static const uint32_t kMinFreeBytes = 600u * 1024u;

static const int kShotW = 320;
static const int kShotH = 240;

static uint16_t *s_buf = nullptr;      // kShotW * kShotH RGB565, PSRAM
static volatile bool s_capturing = false;
static uint32_t s_armedAtMs = 0; // for the watchdog below
static volatile bool s_ready = false;  // buffer full, waiting to be written
static volatile bool s_failed = false; // could not allocate, or could not write
static char s_lastPath[32] = {0};

extern "C" bool tdeck_shot_capturing(void)
{
    return s_capturing;
}

extern "C" const char *tdeck_shot_last_path(void)
{
    return s_lastPath;
}

extern "C" bool tdeck_shot_failed(void)
{
    return s_failed;
}

// Called from the UI once the countdown expires. Allocates on first use and keeps the
// buffer: 150 KB of PSRAM held for the life of the session is a fair price for not
// risking an allocation failure at the moment somebody is trying to capture a bug.
extern "C" void tdeck_shot_begin(void)
{
#if SHOT_HAVE_FS
    if (s_capturing || s_ready)
        return;
    if (!s_buf) {
        s_buf = (uint16_t *)heap_caps_malloc((size_t)kShotW * kShotH * 2, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            s_failed = true;
            return;
        }
    }
    memset(s_buf, 0, (size_t)kShotW * kShotH * 2);
    s_failed = false;
    s_armedAtMs = millis();
    s_capturing = true;
#endif
}

// Called from the LVGL flush callback for every area, BEFORE the byte swap that the
// panel wants - so what lands here is plain native RGB565.
//
// Areas are clipped rather than assumed to be in range: a rounder callback on some
// panels widens them, and LVGL is entitled to flush an area larger than it was asked to.
extern "C" void tdeck_shot_capture_area(int x1, int y1, int x2, int y2, const uint16_t *px)
{
#if SHOT_HAVE_FS
    if (!s_capturing || !s_buf || !px)
        return;
    const int aw = x2 - x1 + 1;
    for (int y = y1; y <= y2; y++) {
        if (y < 0 || y >= kShotH)
            continue;
        for (int x = x1; x <= x2; x++) {
            if (x < 0 || x >= kShotW)
                continue;
            s_buf[y * kShotW + x] = px[(y - y1) * aw + (x - x1)];
        }
    }
#else
    (void)x1; (void)y1; (void)x2; (void)y2; (void)px;
#endif
}

// The last area of a refresh has gone past: the buffer now holds a whole frame.
extern "C" void tdeck_shot_frame_done(void)
{
    if (s_capturing) {
        s_capturing = false;
        s_ready = true;
    }
}

#if SHOT_HAVE_FS
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static bool writeBmp(const char *path)
{
    const int rowBytes = kShotW * 3;
    const int pad = (4 - (rowBytes % 4)) % 4; // BMP rows are padded to 4 bytes
    const uint32_t dataLen = (uint32_t)(rowBytes + pad) * kShotH;

    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f)
        return false;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, 54 + dataLen); // file size
    put32(hdr + 10, 54);          // pixel data offset
    put32(hdr + 14, 40);          // DIB header size
    put32(hdr + 18, kShotW);
    put32(hdr + 22, kShotH); // positive height = bottom-up, which is BMP's default
    hdr[26] = 1;             // planes
    hdr[28] = 24;            // bits per pixel
    put32(hdr + 34, dataLen);
    put32(hdr + 38, 2835); // 72 dpi, in pixels per metre
    put32(hdr + 42, 2835);
    if (f.write(hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        f.close();
        return false;
    }

    // One row at a time: a second full-size buffer for the 24-bit copy would cost another
    // 230 KB for no reason. Bottom-up, because that is the direction BMP stores rows.
    static uint8_t row[kShotW * 3 + 4];
    for (int y = kShotH - 1; y >= 0; y--) {
        const uint16_t *src = s_buf + (size_t)y * kShotW;
        uint8_t *d = row;
        for (int x = 0; x < kShotW; x++) {
            const uint16_t c = src[x];
            // RGB565 -> 8 bits per channel. The high bits are repeated into the low ones so
            // that full-scale stays full-scale (0x1f -> 0xff, not 0xf8).
            const uint8_t r = (uint8_t)(((c >> 11) & 0x1f) * 255 / 31);
            const uint8_t g = (uint8_t)(((c >> 5) & 0x3f) * 255 / 63);
            const uint8_t b = (uint8_t)((c & 0x1f) * 255 / 31);
            *d++ = b; // BMP stores BGR
            *d++ = g;
            *d++ = r;
        }
        for (int p = 0; p < pad; p++)
            *d++ = 0;
        if (f.write(row, rowBytes + pad) != rowBytes + pad) {
            f.close();
            return false;
        }
    }
    f.close();
    return true;
}
#endif

#if SHOT_HAVE_FS
// Keep /shots down to kKeep files. Numbers only ever go up, so the lowest-numbered file is
// the oldest - no directory timestamps needed, which LittleFS does not give us anyway.
static void pruneShots(void)
{
    int present[1000];
    int count = 0;
    File dir = FSCom.open("/shots", FILE_O_READ);
    if (!dir)
        return;
    for (File e = dir.openNextFile(); e && count < 1000; e = dir.openNextFile()) {
        const char *nm = e.name();
        int idx = 0;
        // e.name() is sometimes the bare name and sometimes the full path, depending on the
        // core version. Take whatever follows the last slash and parse that.
        const char *slash = strrchr(nm, '/');
        if (slash)
            nm = slash + 1;
        if (sscanf(nm, "shot%d.bmp", &idx) == 1)
            present[count++] = idx;
        e.close();
    }
    dir.close();
    if (count <= kKeep)
        return;
    // Simple selection: repeatedly remove the smallest index until we are at the cap.
    for (int gone = 0; gone < count - kKeep; gone++) {
        int lo = -1, at = -1;
        for (int i = 0; i < count; i++)
            if (present[i] >= 0 && (lo < 0 || present[i] < lo)) {
                lo = present[i];
                at = i;
            }
        if (at < 0)
            break;
        present[at] = -1;
        char p[32];
        snprintf(p, sizeof(p), "/shots/shot%03d.bmp", lo);
        FSCom.remove(p);
    }
}
#endif

// Main loop. Writes the pending shot, if there is one.
extern "C" void tdeck_shot_service(void)
{
#if SHOT_HAVE_FS
    // Watchdog. The capture completes when a whole frame has been flushed past; if for any
    // reason one never is, s_capturing would stay set and every later screenshot would be
    // refused by the guard in tdeck_shot_begin(). Two seconds is many frames.
    if (s_capturing && (millis() - s_armedAtMs) > 2000) {
        s_capturing = false;
        s_failed = true;
        return;
    }
    if (!s_ready)
        return;
    s_ready = false;

    // No spiLock here, deliberately: internal flash has its own controller and is not on the
    // bus the display, the card and the radio share. That is a real advantage of moving off
    // the SD card, not an oversight.
    FSCom.mkdir("/shots");
    pruneShots();

    const uint32_t total = FSCom.totalBytes();
    const uint32_t used = FSCom.usedBytes();
    if (total > used && (total - used) < kMinFreeBytes) {
        // Say so rather than half-writing a file and leaving the filesystem full.
        snprintf(s_lastPath, sizeof(s_lastPath), "%s", "");
        s_failed = true;
        return;
    }

    char path[32];
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), "/shots/shot%03d.bmp", n);
        if (!FSCom.exists(path))
            break;
    }
    if (n >= 1000) {
        s_failed = true;
        return;
    }
    if (writeBmp(path)) {
        snprintf(s_lastPath, sizeof(s_lastPath), "%s", path);
        s_failed = false;
    } else {
        FSCom.remove(path); // never leave a truncated file behind
        s_failed = true;
    }
#endif
}
