// -----------------------------------------------------------------------------
// TDeckNet — the "internet door" for Lua apps (firmware side).
//
// Lets an app fetch a URL over Wi-Fi WITHOUT freezing the screen and WITHOUT the
// Wi-Fi/Bluetooth crash. The app never blocks: it calls net.fetch(url), keeps
// drawing, and checks net.status()/net.body() on later frames.
//
// THREADING: the LVGL/app task must not drive the radio (Wi-Fi/BT) or block on a
// network read — that is exactly what froze the device before. So the app-facing
// calls only RECORD intent and READ results; tdeck_net_service(), called from the
// main loop(), does the real work on the safe thread as a non-blocking state
// machine. Same deferred pattern as TDeckMeshSwitch / TDeckGpsControl / TDeckClockFormat.
//
// Wi-Fi and Bluetooth share the one 2.4GHz radio on the ESP32-S3, and bringing Wi-Fi
// up while BT is live is what caused the watchdog reboots (the open Get-Apps/BT freeze
// bug). So this shuts Bluetooth down FIRST. BT stays down until the next reboot — the
// chip can't run both well at once, and cleanly re-enabling BT afterwards is a follow-up.
// -----------------------------------------------------------------------------
#include "configuration.h"

#if HAS_WIFI
#include "NodeDB.h" // config.network.wifi_ssid / wifi_psk
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#endif

// The launcher's on-demand Wi-Fi bridge (src/TDeckWifi.cpp) — reuse it rather than
// duplicate WiFi.begin/status/off.
extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk);
extern "C" void tdeck_wifi_disconnect_now(void);
extern "C" bool tdeck_wifi_connected(void);
// Shut Bluetooth down before Wi-Fi comes up (defined in src/modules/AdminModule.cpp).
// Safe to call even if BT is already down (deinit is a no-op then).
void disableBluetooth();

enum NetState { NET_IDLE = 0, NET_START, NET_CONNECTING, NET_FETCH, NET_DONE, NET_ERROR };

static volatile int s_state = NET_IDLE;
static volatile bool s_pending = false; // a fresh request from the app task
static char s_url[256];
static uint32_t s_deadline = 0;
static char *s_body = nullptr; // PSRAM; holds the response body while NET_DONE
static volatile int s_bodyLen = 0;
static const int kNetMaxBody = 8192; // app responses are small (weather JSON ~1-2KB)

// ---- app-facing (called on the LVGL/app task) --------------------------------

// net.fetch(url) -> bool. Starts a fetch. false if one's already running, the URL is
// empty, or no Wi-Fi network is saved (the app should then tell the user to set Wi-Fi up).
extern "C" bool tdeck_net_fetch(const char *url)
{
#if HAS_WIFI
    if (!url || !url[0])
        return false;
    if (s_pending || s_state == NET_START || s_state == NET_CONNECTING || s_state == NET_FETCH)
        return false; // busy
    if (config.network.wifi_ssid[0] == 0)
        return false; // no saved network
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
    s_pending = true; // handed to the main-loop service
    return true;
#else
    (void)url;
    return false;
#endif
}

// 0 idle, 1 working, 2 done, 3 error.
extern "C" int tdeck_net_poll(void)
{
    switch (s_state) {
    case NET_START:
    case NET_CONNECTING:
    case NET_FETCH:
        return 1;
    case NET_DONE:
        return 2;
    case NET_ERROR:
        return 3;
    default:
        return 0;
    }
}

// Copy the body when done; returns bytes copied (0 if not done). Reading it returns the
// door to idle so the next fetch can start. Only safe to read once NET_DONE, which the
// service sets AFTER the body is fully written — so no torn reads.
extern "C" int tdeck_net_result(char *buf, int cap)
{
    if (s_state != NET_DONE || !buf || cap <= 0)
        return 0;
    int n = s_bodyLen;
    if (n > cap - 1)
        n = cap - 1;
#if HAS_WIFI
    if (s_body && n > 0)
        memcpy(buf, s_body, n);
#endif
    buf[n] = 0;
#if HAS_WIFI
    if (s_body) {
        heap_caps_free(s_body);
        s_body = nullptr;
    }
#endif
    s_bodyLen = 0;
    s_state = NET_IDLE;
    return n;
}

// Clear an error/done state without reading (so the app can retry).
extern "C" void tdeck_net_reset(void)
{
#if HAS_WIFI
    if (s_state == NET_DONE || s_state == NET_ERROR) {
        if (s_body) {
            heap_caps_free(s_body);
            s_body = nullptr;
        }
        s_bodyLen = 0;
        s_state = NET_IDLE;
    }
#endif
}

// ---- main-loop service (the safe thread) -------------------------------------
#if HAS_WIFI
static WiFiClientSecure *s_client = nullptr;

// Blocking HTTPS GET into a fresh PSRAM buffer, run ONLY once Wi-Fi is already up and
// with short timeouts, so it can't wedge the loop for long. Mirrors getappsGet.
static bool netHttpGet(const char *url)
{
    if (!s_client) {
        s_client = new WiFiClientSecure();
        s_client->setInsecure(); // public, read-only APIs; no room for a CA bundle
    }
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    http.setUserAgent("t-ui-tdeck"); // some APIs reject a blank/odd user-agent
    LOG_INFO("net: GET %s", url);
    if (!http.begin(*s_client, url)) {
        LOG_INFO("net: http.begin() failed");
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG_INFO("net: GET failed, code=%d", code);
        http.end();
        return false;
    }
    // Read the body. Open-Meteo (and many APIs) send it CHUNKED with no Content-Length header,
    // so http.getSize() returns -1 — that's WHY the first version failed, it treated -1 as empty.
    // getString() reads and de-chunks the whole body cleanly; our JSON responses are small.
    (void)http.getSize();
    String body = http.getString();
    http.end();
    int got = (int)body.length();
    LOG_INFO("net: read %d bytes", got);
    if (got <= 0 || got > kNetMaxBody)
        return false;
    char *buf = (char *)heap_caps_malloc(got + 1, MALLOC_CAP_SPIRAM);
    if (!buf)
        return false;
    memcpy(buf, body.c_str(), (size_t)got);
    buf[got] = 0;
    if (s_body)
        heap_caps_free(s_body);
    s_body = buf;
    s_bodyLen = got;
    return true;
}
#endif

extern "C" void tdeck_net_service(void)
{
#if HAS_WIFI
    if (s_pending && (s_state == NET_IDLE || s_state == NET_DONE || s_state == NET_ERROR)) {
        s_pending = false;
        s_state = NET_START;
    }

    switch (s_state) {
    case NET_START: {
        // Wi-Fi and BT can't share the radio here — drop BT first, but only ONCE. Calling
        // deinit() over and over on an already-down stack is needless and a place double-frees
        // could hide, so latch it. BT stays down until the next reboot regardless.
        static bool s_btDown = false;
        if (!s_btDown) {
            disableBluetooth();
            s_btDown = true;
        }
        LOG_INFO("net: START, firmware wifi ssid='%s'", config.network.wifi_ssid);
        if (!tdeck_wifi_connect_now(config.network.wifi_ssid, config.network.wifi_psk)) {
            LOG_INFO("net: connect_now failed (no ssid on the firmware side?)");
            s_state = NET_ERROR;
            break;
        }
        s_deadline = millis() + 15000; // give the join up to 15s
        s_state = NET_CONNECTING;
        break;
    }
    case NET_CONNECTING: {
        if (tdeck_wifi_connected()) {
            LOG_INFO("net: wifi connected");
            delay(500); // let DHCP/DNS settle before the first TLS connect
            s_state = NET_FETCH;
        } else if ((int32_t)(millis() - s_deadline) > 0) {
            LOG_INFO("net: wifi connect TIMED OUT after 15s");
            tdeck_wifi_disconnect_now();
            s_state = NET_ERROR;
        }
        break;
    }
    case NET_FETCH: {
        bool ok = netHttpGet(s_url);
        tdeck_wifi_disconnect_now(); // done with Wi-Fi; power it back down
        s_state = ok ? NET_DONE : NET_ERROR;
        LOG_INFO("net: result = %s", ok ? "DONE" : "FAILED");
        break;
    }
    default:
        break;
    }
#endif
}
