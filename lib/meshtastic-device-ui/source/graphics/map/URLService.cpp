#include "graphics/map/URLService.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/TileProvider.h"
#include "lvgl.h"
#include "util/ILog.h"

#ifdef ARDUINO_ARCH_ESP32

#include "HTTPClient.h" // not available on Linux/Portduino
#include "WiFi.h"
#include "WiFiClientSecure.h"

// from ConvertPNG.c
extern "C" {
bool decodeImgGrey(const void *data, size_t size, lv_img_dsc_t **img);
bool decodeImgColor(const void *data, size_t size, lv_img_dsc_t **img);
}

URLService::URLService(Callback cb) : ITileService("HTTP:"), saveCB(cb) {}

URLService::~URLService() {}

bool URLService::load(const char *name, void *img)
{
    HTTPClient http;

    if (WiFi.status() != WL_CONNECTED) {
        ILOG_DEBUG("URLService::load skipped (WiFi not connected)");
        return false;
    }

    // Browse-fill runs on the UI task, so a fetch is a screen freeze for its whole
    // duration (TLS handshake + GET). It's a bonus feature, never worth wrecking the
    // panning feel: skip while any input is held down (mid-drag/mid-press) and allow
    // at most ~2 attempts a second. Skipped tiles show the no-tile image and
    // MapPanel's retry sweep picks them up once the user goes idle.
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
            return false;
    }
    static uint32_t s_lastAttempt = 0;
    uint32_t now = lv_tick_get();
    if (now - s_lastAttempt < 500)
        return false;
    s_lastAttempt = now;

    struct LvFreeGuard {
        uint8_t *&ptr;
        ~LvFreeGuard() { lv_free(ptr); }
    };

    // transform filename to provider url
    std::string url = TileProvider::url(name);
    if (url.empty()) {
        ILOG_ERROR("empty URL for tile %s", name ? name : "(null)");
        return false;
    }

    http.setReuse(false);
    // hard caps: a dead server may cost at most ~2.5s of UI, not the 5s+ defaults
    http.setConnectTimeout(2500);
    http.setTimeout(2500);
    bool began;
    if (strncmp(url.c_str(), "https", 5) == 0) {
        // https tile servers (Google, USGS): TLS without cert pinning — it's public
        // map data, and the S3 has no room for a CA bundle here.
        static WiFiClientSecure secureClient;
        secureClient.setInsecure();
        began = http.begin(secureClient, url.c_str());
    } else {
        began = http.begin(url.c_str());
    }
    if (!began) {
        ILOG_ERROR("ERROR begin %s", url.c_str());
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        ILOG_ERROR("ERROR GET %s : %d", url.c_str(), httpCode);
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    int contentLen = http.getSize();
    if (contentLen <= 0) {
        ILOG_WARN("GET %s : empty", url.c_str());
        return false;
    }

    size_t len = (size_t)contentLen;

    uint8_t *pngImage = (uint8_t *)lv_malloc(len);
    LvFreeGuard pngGuard{pngImage};
    if (!pngImage) {
        ILOG_ERROR("lv_malloc failed for %s (%u bytes)", url.c_str(), (unsigned int)len);
        return false;
    }

    // read .png file in chunks to increase reliability (avoid readBytes())
    size_t bytesRead = 0;
    uint8_t idleSpins = 0;
    const uint8_t maxIdleSpins = 3;
    while (bytesRead < len) {
        size_t available = stream->available();
        if (available == 0) {
            if (++idleSpins > maxIdleSpins) {
                break;
            }
            delay(5);
            continue;
        }

        idleSpins = 0;
        size_t toRead = available;
        size_t remaining = len - bytesRead;
        if (toRead > remaining) {
            toRead = remaining;
        }

        int got = stream->read(pngImage + bytesRead, toRead);
        if (got <= 0) {
            break;
        }
        bytesRead += (size_t)got;
    }

    if (bytesRead != len) {
        ILOG_ERROR("http read error %s : %u != %u", url.c_str(), (unsigned int)bytesRead, (unsigned int)len);
        return false;
    }

    ILOG_DEBUG("SUCCESS(%d): GET %s (%u bytes)", (int)idleSpins, url.c_str(), (unsigned int)len);

    // save png tile to SD card
    if (saveCB && MapTileSettings::saveOK()) {
        bool result = saveCB(name, pngImage, len);
        ILOG_DEBUG("save png to SD -> %s", result ? "OK" : "failed");
    }

    // decode png via STBI library
    lv_img_dsc_t *img_dsc = nullptr;
    bool decoded = MapTileSettings::color() ? decodeImgColor(pngImage, len, &img_dsc) : decodeImgGrey(pngImage, len, &img_dsc);
    if (decoded) {
        lv_obj_t *img_obj = (lv_obj_t *)img;
        lv_image_set_src(img_obj, img_dsc);
        if (lv_image_get_src(img_obj) != img_dsc) {
            ILOG_ERROR("lv_image_set_src failed for tile %s", name);
            if (img_dsc->data && img_dsc->data_size > 0) {
                lv_free((void *)img_dsc->data);
            }
            lv_free(img_dsc);
            return false;
        }
    } else {
        ILOG_ERROR("Failed to decode tile image %s", name);
        return false;
    }

    return true;
}

#endif