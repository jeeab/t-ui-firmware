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
// HOW IT WRITES. Not from the flush callback. That runs on the LVGL task mid-refresh,
// and pushing 230 KB at the SD card from there is the SPI contention problem in its
// purest form. The callback only fills the buffer and sets a flag; tdeck_shot_service()
// on the main loop does the writing.
//
// Files land in /shots as shot001.bmp upward. 24-bit BMP: bigger than it needs to be,
// but every computer and phone opens it with no thought, which is the whole point.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <cstdio>
#include <cstring>

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
#include "graphics/common/SdCard.h"
#include <esp_heap_caps.h>
#define SHOT_HAVE_SD 1
#else
#define SHOT_HAVE_SD 0
#endif

static const int kShotW = 320;
static const int kShotH = 240;

static uint16_t *s_buf = nullptr;      // kShotW * kShotH RGB565, PSRAM
static volatile bool s_arming = false; // waiting for the countdown to finish
static volatile bool s_capturing = false;
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
#if SHOT_HAVE_SD
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
#if SHOT_HAVE_SD
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

#if SHOT_HAVE_SD
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

    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
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

// Main loop. Writes the pending shot, if there is one.
extern "C" void tdeck_shot_service(void)
{
#if SHOT_HAVE_SD
    if (!s_ready)
        return;
    s_ready = false;

    SDFs.mkdir("/shots");
    char path[32];
    // Find the first free number rather than keeping a counter: the card outlives any
    // counter we could hold in RAM, and overwriting somebody's evidence would be rude.
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), "/shots/shot%03d.bmp", n);
        if (!SDFs.exists(path))
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
        s_failed = true;
    }
#endif
}
