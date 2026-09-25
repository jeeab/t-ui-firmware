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
extern "C" int tdeck_net_state(void); // raw NetState, for the progress text
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
// ⭐ WHERE TO GO WHEN THE GOOD MODEL IS BUSY. MEASURED 2026-09-25 from the PC on Jake's own key,
// three attempts each, while his T-Deck was getting the same 503s:
//     gemini-flash-latest        503  503  503
//     gemini-flash-lite-latest   200  200  200
//     gemini-2.0-flash / 2.5-flash   404 (not available to this key at all)
// So a 503 here is not a blip to wait out, it is a model that is unavailable to him right now.
// Answering with the lite model beats an error message, and it is the same shape as the 404
// fallback that already exists two functions down.
const char *kBusyFallbackModel = "gemini-flash-lite-latest";

// ⛔ THESE ARE tdeck_net_poll()'s RETURN CODES, NOT TDeckNet's internal NetState enum.
//
// They used to be the enum - NET_DONE = 4, NET_ERROR = 5 - "mirroring TDeckNet's enum". But
// tdeck_net_poll() does not return that enum. It COLLAPSES it: "0 idle, 1 working, 2 done,
// 3 error". So the comparisons could never match, service() fell through to "keep waiting" on
// every single tick, and Gemini sat on "Connecting..." forever while the answer was sitting
// there waiting to be read.
//
// Jake found it the day he first had a key to try: "asking it 'hello' is just stuck at
// connecting". It had never worked, from the commit that added the app. Mirroring an enum
// across a module boundary is exactly the kind of duplication that rots silently - mirror what
// the FUNCTION returns, and name the constants after the function so the next person cannot
// make the same swap.
constexpr int POLL_IDLE = 0, POLL_WORKING = 1, POLL_DONE = 2, POLL_ERROR = 3;
// And these ARE the raw NetState, only ever compared against tdeck_net_state().
constexpr int RAW_CONNECTING = 2, RAW_FETCH = 3;

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

// ⛔ THE SIZE HAS TO BE A NAMED CONSTANT, not sizeof(). This buffer used to be `char buf[4096]`,
// where sizeof(buf) was 4096. Moving it to PSRAM made it a POINTER, and every sizeof(buf) left
// behind silently became 4 - so the HTTP reply was read 4 bytes at a time and Gemini stopped
// working. Nothing warns about it; the code still compiles and still looks right.
static const int kWorkBuf = 4096;
static char *s_workBuf = nullptr;

static char *geminiWorkBuf(void)
{
    if (!s_workBuf) {
        s_workBuf = (char *)heap_caps_malloc(kWorkBuf, MALLOC_CAP_SPIRAM);
        if (!s_workBuf)
            s_workBuf = (char *)malloc(kWorkBuf);
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
int gRetries = 0;         // 503/429 attempts used on THIS question
uint32_t gRetryAtMs = 0;  // when to fire the next one; 0 = nothing pending

// ---- /gemini.txt ---------------------------------------------------------------------------
// key=... and model=..., one per line, '#' comments. The same file the Max uses, so an SD card
// can be moved between the two devices and simply work.
// ⛔ A FAILED SD READ IS NOT PROOF THE KEY IS ABSENT, AND MUST NOT BE CACHED.
//
// This set gConfigRead = true BEFORE opening the file, so a single failed open - the card shares
// its SPI bus with the display and the radio, and readConfig() runs early in boot when it may not
// even be mounted yet - latched "no key" for the rest of the session. Jake's key was on the card
// the whole time; the device had decided otherwise once, seconds after power-on, and never looked
// again.
//
// This is the FOURTH time today: Mail's haveCreds() asking him to log in every time, the map style
// list saying "no styles on card", the app icon reader, and now this. The shape is always the same
// - an unreliable read whose failure is recorded as a fact. Only cache a SUCCESS.
void readConfig()
{
    if (gConfigRead)
        return;
    FsFile f = SDFs.open("/gemini.txt", O_RDONLY);
    if (!f) {
        delay(20); // the other user of the bus is mid-transfer; give it a moment
        f = SDFs.open("/gemini.txt", O_RDONLY);
    }
    if (!f) {
        LOG_INFO("[GEMINI] could not read /gemini.txt - will try again next time, not caching this");
        return; // deliberately NOT setting gConfigRead: we do not know, so we will ask again
    }
    gConfigRead = true;
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
    gRetries = 0;    // a new question gets its own budget of busy-retries
    gRetryAtMs = 0;  // and must never inherit a pending one from the last
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

    // A 503/429 retry that is waiting out its backoff. Fired from here rather than from inside
    // the error handler for the same reason the 404 retry is: never call the request function
    // from inside a failed request.
    if (gRetryAtMs) {
        if ((int32_t)(millis() - gRetryAtMs) < 0)
            return; // still waiting
        gRetryAtMs = 0;
        startRequest();
        return;
    }

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

    // ⛔ SAY WHICH STAGE IT IS ON. Jake, 2026-09-25: "asking it 'hello' is just stuck at
    // connecting". "Connecting..." was set once in ask() and never touched again until the
    // answer or the error landed - so joining wi-fi, negotiating TLS and waiting on Google all
    // looked identical, and identical to a genuine hang. A request that takes 20 seconds and one
    // that will never finish MUST NOT look the same.
    {
        static int lastNet = -1;
        const int st = tdeck_net_state();
        if (st != lastNet) {
            lastNet = st;
            if (st == RAW_CONNECTING)
                snprintf(gStatus, sizeof(gStatus), "Joining wi-fi...");
            else if (st == RAW_FETCH)
                snprintf(gStatus, sizeof(gStatus), "Asking Gemini...");
        }
    }

    const int net = tdeck_net_poll();
    if (net == POLL_DONE) {
        // ⛔ PSRAM, for the same reason as gReply above: 4KB of internal RAM held permanently
        // for a buffer used only while a Gemini request is in flight. File scope rather than a
        // static local so release() can hand it back when the app is closed.
        char *buf = geminiWorkBuf();
        if (!buf)
            return;
        const int n = tdeck_net_result(buf, kWorkBuf);
        tdeck_net_reset();
        if (n <= 0) {
            fail("Empty answer");
            return;
        }
        buf[n < kWorkBuf ? n : kWorkBuf - 1] = 0;
        char *reply = replyBuf();
        if (!reply) {
            fail("Out of memory reading the answer");
            return;
        }
        if (extractReply(buf, reply, kMaxReply + 1)) {
            snprintf(gStatus, sizeof(gStatus), "");
            gState = DONE;
        } else {
            // extractReply puts Gemini's own explanation in gReply when it refused; show that
            // rather than a generic failure, because it is usually actionable.
            snprintf(gStatus, sizeof(gStatus), "%s", reply[0] ? reply : "Could not read the answer");
            reply[0] = 0;
            gState = FAILED;
        }
        return;
    }

    if (net == POLL_ERROR) {
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
        // ⭐ 503 AND 429 ARE "ASK AGAIN", NOT "IT BROKE". Google's own 503 body says so: "This
        // model is currently experiencing high demand. Spikes in demand are usually temporary.
        // Please try again later." Measured from the PC on the same key the same evening - a
        // 503, then an immediate retry returned a normal answer. Handing Jake "Gemini error 503"
        // and stopping makes a busy minute look like a broken feature, and he cannot tell the
        // difference between the two from the screen.
        //
        // Two attempts, not a loop: if Google is genuinely down, hammering it is both rude and
        // useless, and the honest message is better than a spinner that never ends.
        if ((code == 503 || code == 429) && gRetries < 2) {
            gRetries++;
            LOG_INFO("[GEMINI] HTTP %d (busy) - retry %d of 2", code, gRetries);
            snprintf(gStatus, sizeof(gStatus), "Google is busy - trying again...");
            gRetryAtMs = millis() + 2500; // a short breath; startRequest() fires from service()
            return;
        }
        // Still busy after both retries: the model itself is unavailable, not momentarily loaded.
        // Switch to the lite model, which was answering 200 while this one answered 503.
        if ((code == 503 || code == 429) && strcmp(gModel, kBusyFallbackModel) != 0) {
            LOG_INFO("[GEMINI] '%s' still busy after %d tries - falling back to %s", gModel, gRetries,
                     kBusyFallbackModel);
            snprintf(gModel, sizeof(gModel), "%s", kBusyFallbackModel);
            snprintf(gStatus, sizeof(gStatus), "Busy - trying the faster model...");
            gRetries = 0; // the new model gets its own budget
            gRetryAtMs = millis() + 500;
            return;
        }
        if (code == 400 || code == 403)
            fail("Key rejected - check /gemini.txt");
        else if (code == 429)
            fail("Gemini is busy - try again in a minute");
        else if (code == 503)
            fail("Google is busy right now - try again in a minute");
        else if (code <= 0)
            fail("No network - check wi-fi");
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Gemini error %d", code);
            fail(msg);
        }
        return;
    }
    // still starting / connecting / fetching - keep waiting
}
} // namespace tdeckgemini
