// -----------------------------------------------------------------------------
// Gemini for the colour T-Deck. See TDeckGemini.h for the rules.
//
// Ported from the Max's MaxGemini.cpp, keeping the two things that version paid for:
//
//   * gemini-2.5-flash is RETIRED for new keys - Google's own 404 body says "no longer
//     available to new users". A 404 retries once with gemini-flash-latest, which answers.
//   * THE RETRY IS A LOOP, NOT RECURSION. The Max's first version called itself again from
//     inside the failed request, mid-TLS, and overflowed the stack: "Guru Meditation Error:
//     Core 1 panic'ed (Double exception)", instant reboot on every send.
//
// The network goes through tdeck_net_post() rather than a WiFiClientSecure of its own. That is
// deliberate: HTTPS works on this board only because of a contiguous-RAM reserve taken at boot
// and released just before the handshake, and a second TLS path would starve exactly as the
// Weather app did when it was missing that release.
// -----------------------------------------------------------------------------
#include "configuration.h"

#include "TDeckGemini.h"
#include "graphics/common/SdCard.h" // SDFs, the shared SdFat instance
#include <string.h>

extern "C" bool tdeck_net_post(const char *url, const char *body, const char *contentType);
extern "C" int tdeck_net_poll(void);
extern "C" int tdeck_net_result(char *buf, int cap);
extern "C" void tdeck_net_reset(void);
extern "C" int tdeck_net_http_code(void);

namespace tdeckgemini
{
namespace
{
constexpr int kMaxPrompt = 220;
constexpr int kMaxReply = 1200;
const char *kFallbackModel = "gemini-flash-latest";

// Net states, mirroring TDeckNet's enum. Kept as plain ints because that module does not export
// its enum, and duplicating the values is less bad than exposing its internals.
constexpr int NET_IDLE = 0, NET_DONE = 4, NET_ERROR = 5;

char gKey[80] = {0};
char gModel[48] = {0};
bool gConfigRead = false;

char gPrompt[kMaxPrompt + 1] = {0};
// ⛔ PSRAM. This is reply TEXT - cold, large, and read only while the Gemini screen is up.
// In internal RAM it was 1,201 bytes of the very thing a TLS handshake needs contiguously.
// Allocated on first use; every reader already tolerates an empty string.
char *gReply = nullptr;

// Give the buffers back. Safe to call repeatedly; the accessors reallocate on next use.
// ⛔ Caller must ensure no request is in flight - see geminiIdleCheck() in GeminiApp.cpp.
void release(void);

static char *s_workBuf = nullptr;

static char *geminiWorkBuf(void)
{
    if (!s_workBuf) {
        s_workBuf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!s_workBuf)
            s_workBuf = (char *)malloc(4096);
    }
    return s_workBuf;
}

static void geminiReleaseWork(void)
{
    if (s_workBuf) {
        heap_caps_free(s_workBuf);
        s_workBuf = nullptr;
    }
}

static char *replyBuf(void)
{
    if (!gReply) {
        gReply = (char *)heap_caps_calloc(kMaxReply + 1, 1, MALLOC_CAP_SPIRAM);
        if (!gReply)
            gReply = (char *)calloc(kMaxReply + 1, 1); // tiny boards / no PSRAM
    }
    return gReply;
}
char gStatus[96] = {0};
int gState = IDLE;
bool gAskPending = false;
bool gRetryOtherModel = false;

// ---- /gemini.txt ---------------------------------------------------------------------------
// key=... and model=..., one per line, '#' comments. The same file the Max uses, so an SD card
// can be moved between the two devices and simply work.
void readConfig()
{
    if (gConfigRead)
        return;
    gConfigRead = true;
    FsFile f = SDFs.open("/gemini.txt", O_RDONLY);
    if (!f) {
        LOG_INFO("[GEMINI] no /gemini.txt on the card");
        return;
    }
    char line[160];
    while (f.available()) {
        int n = f.fgets(line, sizeof(line));
        if (n <= 0)
            break;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !*p)
            continue;
        if (!strncmp(p, "key=", 4))
            snprintf(gKey, sizeof(gKey), "%s", p + 4);
        else if (!strncmp(p, "model=", 6))
            snprintf(gModel, sizeof(gModel), "%s", p + 6);
    }
    f.close();
    if (!gModel[0])
        snprintf(gModel, sizeof(gModel), "%s", kFallbackModel);
    LOG_INFO("[GEMINI] config: key=%s model=%s", gKey[0] ? "yes" : "MISSING", gModel);
}

void fail(const char *why)
{
    snprintf(gStatus, sizeof(gStatus), "%s", why);
    gState = FAILED;
    LOG_WARN("[GEMINI] %s", why);
}

// ---- JSON in and out -------------------------------------------------------------------------
void jsonEscape(const char *in, char *out, int outSize)
{
    int o = 0;
    for (const char *p = in; *p && o < outSize - 8; p++) {
        switch (*p) {
        case '"':
            out[o++] = '\\';
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            out[o++] = '\\';
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            break;
        case '\t':
            out[o++] = ' ';
            break;
        default:
            if ((unsigned char)*p >= 0x20)
                out[o++] = *p;
        }
    }
    out[o] = 0;
}

void jsonUnescapeInto(const char *p, char *out, int outSize)
{
    int o = 0;
    for (; *p && *p != '"' && o < outSize - 1; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':
                out[o++] = '\n';
                break;
            case 't':
                out[o++] = ' ';
                break;
            case 'u': // \uXXXX - not worth a decoder here; skip the code point
                if (p[1] && p[2] && p[3] && p[4])
                    p += 4;
                break;
            default:
                out[o++] = *p;
            }
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

// The first "text": inside candidates is the answer. A refusal or a bad request carries
// "message" instead, and saying WHICH is the difference between a fixable problem and a shrug.
bool extractReply(const char *body, char *out, int outSize)
{
    const char *t = strstr(body, "\"text\":");
    if (t) {
        const char *q = strchr(t + 7, '"');
        if (q) {
            jsonUnescapeInto(q + 1, out, outSize);
            return out[0] != 0;
        }
    }
    const char *m = strstr(body, "\"message\":");
    if (m) {
        const char *q = strchr(m + 10, '"');
        if (q) {
            char msg[160];
            jsonUnescapeInto(q + 1, msg, sizeof(msg));
            snprintf(out, outSize, "Gemini said no: %s", msg);
        }
    }
    return false;
}

bool startRequest()
{
    static char url[300];
    snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s",
             gModel, gKey);

    // Static, not stack: this is a big buffer and the net task's stack is not the place for it.
    static char esc[kMaxPrompt * 2 + 8];
    jsonEscape(gPrompt, esc, sizeof(esc));

    // The system instruction earns its place: 320x240 with no markdown renderer, so asterisks
    // and bullets arrive as literal clutter.
    static char body[kMaxPrompt * 2 + 640];
    snprintf(body, sizeof(body),
             "{\"system_instruction\":{\"parts\":[{\"text\":\"You are on a small handheld device with a "
             "320x240 screen. Reply in plain text only: no markdown, no asterisks, no bullet characters, "
             "no emoji. Be accurate and brief - at most 3 short sentences unless asked for more.\"}]},"
             "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}],"
             "\"generationConfig\":{\"maxOutputTokens\":400,\"temperature\":0.7}}",
             esc);

    LOG_INFO("[GEMINI] asking %s (%d char prompt)", gModel, (int)strlen(gPrompt));
    if (!tdeck_net_post(url, body, "application/json")) {
        fail("Could not start the request");
        return false;
    }
    return true;
}
} // namespace

// ---- public ----------------------------------------------------------------------------------
void ask(const char *prompt)
{
    if (!prompt || !*prompt)
        return;
    snprintf(gPrompt, sizeof(gPrompt), "%s", prompt);
    if (replyBuf())
        gReply[0] = 0;
    gRetryOtherModel = false;
    snprintf(gStatus, sizeof(gStatus), "Connecting...");
    gState = WORKING;
    gAskPending = true; // the request itself starts on the next service() tick
}

int state()
{
    return gState;
}
const char *reply()
{
    return gReply;
}
const char *statusText()
{
    return gStatus;
}
void release(void)
{
    if (gReply) {
        heap_caps_free(gReply);
        gReply = nullptr;
    }
    geminiReleaseWork();
}

void clear()
{
    if (replyBuf())
        gReply[0] = 0;
    gStatus[0] = 0;
    gState = IDLE;
    tdeck_net_reset();
}

bool haveConfig()
{
    readConfig();
    return gKey[0] != 0;
}

void service()
{
    if (gState != WORKING)
        return;

    if (gAskPending) {
        gAskPending = false;
        readConfig();
        if (!gKey[0]) {
            fail("No key - put one in /gemini.txt on the SD card");
            return;
        }
        startRequest();
        return;
    }

    const int net = tdeck_net_poll();
    if (net == NET_DONE) {
        // ⛔ PSRAM, for the same reason as gReply above: 4KB of internal RAM held permanently
        // for a buffer used only while a Gemini request is in flight. File scope rather than a
        // static local so release() can hand it back when the app is closed.
        char *buf = geminiWorkBuf();
        if (!buf)
            return;
        const int n = tdeck_net_result(buf, sizeof(buf));
        tdeck_net_reset();
        if (n <= 0) {
            fail("Empty answer");
            return;
        }
        buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = 0;
        if (extractReply(buf, gReply, sizeof(gReply))) {
            snprintf(gStatus, sizeof(gStatus), "");
            gState = DONE;
        } else {
            // extractReply puts Gemini's own explanation in gReply when it refused; show that
            // rather than a generic failure, because it is usually actionable.
            snprintf(gStatus, sizeof(gStatus), "%s", gReply[0] ? gReply : "Could not read the answer");
            if (replyBuf())
        gReply[0] = 0;
            gState = FAILED;
        }
        return;
    }

    if (net == NET_ERROR) {
        const int code = tdeck_net_http_code();
        tdeck_net_reset();
        // ⛔ A LOOP, NEVER RECURSION. The Max's first version retried by calling the request
        // function from inside itself, mid-TLS, and overflowed the stack - a double exception and
        // an instant reboot on every send. Coming back through service() costs nothing and cannot
        // do that.
        if (code == 404 && strcmp(gModel, kFallbackModel) != 0 && !gRetryOtherModel) {
            LOG_INFO("[GEMINI] model '%s' gave 404 - retrying with %s", gModel, kFallbackModel);
            snprintf(gModel, sizeof(gModel), "%s", kFallbackModel);
            gRetryOtherModel = true;
            snprintf(gStatus, sizeof(gStatus), "Trying %s...", kFallbackModel);
            startRequest();
            return;
        }
        if (code == 400 || code == 403)
            fail("Key rejected - check /gemini.txt");
        else if (code == 429)
            fail("Too many requests - wait a moment");
        else if (code <= 0)
            fail("No network - check wi-fi");
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Gemini error %d", code);
            fail(msg);
        }
        return;
    }
    // still NET_START / NET_CONNECTING / NET_FETCH - keep waiting
}
} // namespace tdeckgemini
