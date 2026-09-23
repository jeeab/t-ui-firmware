#include "configuration.h"

#if HAS_WIFI

#include "TDeckMail.h"
#include "NodeDB.h" // config.network.wifi_ssid / wifi_psk
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "graphics/common/SdCard.h"

// -----------------------------------------------------------------------------------------
// Gmail over IMAP. STEP ONE ONLY: connect, authenticate, report inbox counts.
//
// Jake, 2026-09-22: "could be a simple email send, inbox, drafts, all that Jaz."
// This is the transport underneath that, built and proven first, because two things could have
// sunk the whole feature and neither is about email:
//
//   1. CERTIFICATE VERIFICATION. tdeck_net_fetch() calls setInsecure() - it does not check who
//      it is talking to - with the comment "no room for a CA bundle". For a public read-only
//      API that is a defensible shortcut. For an account password it is not: on a hostile
//      network anything in the middle could present its own certificate and keep the password.
//      ⭐ We do not need a BUNDLE, only the ONE root Gmail chains to, which is ~1.9KB.
//   2. HEAP. Measured on Jake's device: internal free 37352, largest contiguous block 20468,
//      all-time low 18212. The handshake wants ~34KB contiguous (two ~16.7KB record buffers),
//      which is why tdeck_tls_reserve_init() holds 36KB aside at boot. IMAP pays the same cost
//      as HTTPS, so it uses the same reserve - it must not open a session while a fetch is
//      running, and it releases and retakes the reserve exactly as netHttpGet does.
//
// ⛔ EVERYTHING HERE RUNS FROM loop(), NEVER FROM THE UI TASK. tdeck_mail_service() is called
// from main.cpp's loop alongside tdeck_net_service(). The UI only ever records an intent and
// reads a result. This is the same rule as the rest of the device: the "tft" task holds spiLock
// while it runs, and a blocking socket read on it freezes the screen.
// -----------------------------------------------------------------------------------------

extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk);
extern "C" void tdeck_wifi_disconnect_now(void);
extern "C" bool tdeck_wifi_connected(void);
extern "C" void tdeck_tls_reserve_release(void);
extern "C" void tdeck_tls_reserve_take(void);

// GTS Root R1, fetched from https://pki.goog/repo/certs/gtsr1.pem and checked against the chain
// imap.gmail.com actually serves:
//     imap.gmail.com  ->  WR2  ->  GTS Root R1
// ⚠️ The copy in the served chain has a DIFFERENT sha256 to this one and that is correct, not a
// mismatch: the server sends the variant cross-signed by GlobalSign, for older trust stores.
// Both carry the SAME public key (verified: 7a9f8103eacb08443605ff801479654fd23b67e68a75364da
// ffa25405ce08273), which is the part that has to match - mbedTLS verifies WR2's signature with
// that key and the chain completes. Valid to 2036-06-22.
static const char kGtsRootR1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char *kImapHost = "imap.gmail.com";
static const int kImapPort = 993;

// ---- state, all touched only from loop() except the volatile handshake with the UI ----------
enum MailState { MAIL_IDLE = 0, MAIL_START, MAIL_CONNECTING, MAIL_WORK, MAIL_DONE, MAIL_ERROR };
static volatile int s_state = MAIL_IDLE;
static volatile bool s_pending = false;
static bool s_ownWifi = false;
static uint32_t s_deadline = 0;
static int s_total = -1, s_unseen = -1;
static char s_err[96] = {0};

// Credentials live ONLY on the SD card, never compiled in - the same rule as the Gemini key, so
// the installer stays shareable. /gmail.txt is two lines: address, then the 16-character app
// password.
static char s_user[96] = {0};
static char s_pass[64] = {0};

// ⭐ CONNECT-ONLY MODE. The two things that could sink this feature - does certificate
// verification actually work, and does the handshake fit in the heap - have nothing to do with
// anyone's password. This mode does the TLS connect, checks the certificate, reads the IMAP
// greeting and logs out, so the risky half can be PROVEN before a credential exists anywhere.
// It is also the diagnostic to reach for later when mail breaks: it separates "the network or
// the certificate is wrong" from "the login is wrong", which otherwise look identical.
static volatile bool s_connectOnly = false;

static void mailFail(const char *why)
{
    strncpy(s_err, why, sizeof(s_err) - 1);
    s_err[sizeof(s_err) - 1] = 0;
    s_state = MAIL_ERROR;
    LOG_INFO("mail: %s", why);
}

// ⭐ GOOGLE SHOWS THE APP PASSWORD AS "abcd efgh ijkl mnop" AND THE SPACES ARE NOT PART OF IT.
// Anyone typing it onto the card copies it the way it is displayed, so strip whitespace rather
// than hand the user a login failure they cannot possibly diagnose.
static void stripSpaces(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n')
            *w++ = *r;
    *w = 0;
}

static bool loadCreds(void)
{
    if (s_user[0] && s_pass[0])
        return true;
    FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
    if (!f) {
        mailFail("no /gmail.txt on the SD card");
        return false;
    }
    int n = f.fgets(s_user, sizeof(s_user));
    int m = n > 0 ? f.fgets(s_pass, sizeof(s_pass)) : 0;
    f.close();
    stripSpaces(s_user);
    stripSpaces(s_pass);
    if (!s_user[0] || !s_pass[0]) {
        s_user[0] = s_pass[0] = 0;
        mailFail("/gmail.txt needs two lines: address, then app password");
        return false;
    }
    (void)m;
    return true;
}

// Read one CRLF line from the socket. Returns false on timeout so a stalled server cannot wedge
// the loop; every caller treats that as a failed session rather than retrying forever.
static bool imapLine(WiFiClientSecure &c, char *buf, int cap, uint32_t timeoutMs = 8000)
{
    int i = 0;
    uint32_t end = millis() + timeoutMs;
    while ((int32_t)(millis() - end) < 0) {
        if (!c.connected() && !c.available())
            break;
        int ch = c.read();
        if (ch < 0) {
            delay(5);
            continue;
        }
        if (ch == '\n') {
            while (i > 0 && buf[i - 1] == '\r')
                i--;
            buf[i] = 0;
            return true;
        }
        if (i < cap - 1)
            buf[i++] = (char)ch;
    }
    buf[0] = 0;
    return false;
}

// Send "<tag> <cmd>" and read until the line that starts with that tag. Returns true on OK.
// `capture`, if given, keeps the first untagged line containing `want`, for the STATUS reply.
static bool imapCmd(WiFiClientSecure &c, const char *tag, const char *cmd, const char *want = nullptr,
                    char *capture = nullptr, int capN = 0)
{
    c.printf("%s %s\r\n", tag, cmd);
    char line[320];
    for (int guard = 0; guard < 60; guard++) {
        if (!imapLine(c, line, sizeof(line)))
            return false;
        if (want && capture && strstr(line, want)) {
            strncpy(capture, line, capN - 1);
            capture[capN - 1] = 0;
        }
        // The tagged response ends the command. Anything before it is untagged chatter.
        if (!strncmp(line, tag, strlen(tag)) && line[strlen(tag)] == ' ')
            return strstr(line + strlen(tag), " OK") == line + strlen(tag);
    }
    return false;
}

// One whole IMAP session, blocking, with short timeouts. Wi-Fi is already up when this runs.
static bool imapCheck(void)
{
    // Same reserve the HTTPS path uses, for the same reason and with the same ordering: release
    // before the handshake, retake only after the client is DELETED, because the handshake
    // buffers are held by the client and re-reserving early just fails.
    tdeck_tls_reserve_release();
    bool ok = false;
    WiFiClientSecure *c = new WiFiClientSecure();
    if (!c) {
        mailFail("out of memory for the TLS client");
        tdeck_tls_reserve_take();
        return false;
    }
    // ⭐ THE WHOLE POINT. Not setInsecure(): a password is going over this socket.
    c->setCACert(kGtsRootR1);
    c->setTimeout(8000);
    LOG_INFO("mail: connecting to %s:%d (largest internal block %u)", kImapHost, kImapPort,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!c->connect(kImapHost, kImapPort)) {
        // A certificate failure and a network failure look the same from here, so say both
        // rather than send the user hunting the wrong one.
        mailFail("could not connect (network, or certificate rejected)");
        goto done;
    }
    {
        char line[320];
        if (!imapLine(*c, line, sizeof(line)) || strncmp(line, "* OK", 4)) {
            mailFail("no IMAP greeting");
            goto done;
        }
        LOG_INFO("mail: connected, TLS up and certificate verified");
        if (s_connectOnly) {
            // The point of this mode: the certificate and the heap are now proven. Stop here
            // rather than touching an account at all.
            imapCmd(*c, "a1", "LOGOUT");
            s_total = s_unseen = -1;
            ok = true;
            LOG_INFO("mail: CONNECT-ONLY OK - certificate verified, handshake fits");
            goto done;
        }

        // ⛔ NEVER LOG THE PASSWORD, and never log the command that carries it. That is why this
        // builds the LOGIN line separately instead of going through a logging helper.
        {
            char cmd[192];
            snprintf(cmd, sizeof(cmd), "LOGIN \"%s\" \"%s\"", s_user, s_pass);
            bool li = imapCmd(*c, "a1", cmd);
            memset(cmd, 0, sizeof(cmd)); // do not leave it on the stack
            if (!li) {
                mailFail("login rejected - check the app password, and that 2FA is on");
                goto done;
            }
        }
        LOG_INFO("mail: logged in");

        // STATUS rather than SELECT: one round trip, gives both numbers, and does not mark
        // anything read. RFC 3501 says not to STATUS the mailbox you have SELECTed - we have
        // selected nothing, so this is the clean order.
        char st[320] = {0};
        if (!imapCmd(*c, "a2", "STATUS INBOX (MESSAGES UNSEEN)", "STATUS", st, sizeof(st))) {
            mailFail("STATUS failed");
            goto done;
        }
        const char *m = strstr(st, "MESSAGES ");
        const char *u = strstr(st, "UNSEEN ");
        s_total = m ? atoi(m + 9) : -1;
        s_unseen = u ? atoi(u + 7) : -1;
        imapCmd(*c, "a3", "LOGOUT");
        ok = true;
        LOG_INFO("mail: INBOX total=%d unseen=%d", s_total, s_unseen);
    }
done:
    c->stop();
    delete c;
    tdeck_tls_reserve_take();
    return ok;
}

// ---- app-facing, called from the UI task: record intent, read results -----------------------

extern "C" bool tdeck_mail_check(void)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_pending = true;
    return true;
}

extern "C" bool tdeck_mail_connect_test(void)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = true;
    s_pending = true;
    return true;
}

extern "C" int tdeck_mail_poll(void)
{
    if (s_state == MAIL_DONE)
        return 1;
    if (s_state == MAIL_ERROR)
        return -1;
    return 0;
}

extern "C" void tdeck_mail_counts(int *total, int *unseen)
{
    if (total)
        *total = s_total;
    if (unseen)
        *unseen = s_unseen;
}

extern "C" const char *tdeck_mail_error(void)
{
    return s_err;
}

// ---- the worker, called from loop() ---------------------------------------------------------

extern "C" void tdeck_mail_service(void)
{
    if (s_pending && (s_state == MAIL_IDLE || s_state == MAIL_DONE || s_state == MAIL_ERROR)) {
        s_pending = false;
        s_state = MAIL_START;
    }

    switch (s_state) {
    case MAIL_START: {
        if (!s_connectOnly && !loadCreds())
            break; // loadCreds already set the error and the state
        if (tdeck_wifi_connected()) {
            s_state = MAIL_WORK;
            break;
        }
        if (!tdeck_wifi_connect_now(config.network.wifi_ssid, config.network.wifi_psk)) {
            mailFail("no wi-fi configured");
            break;
        }
        s_ownWifi = true;
        s_deadline = millis() + 15000;
        s_state = MAIL_CONNECTING;
        break;
    }
    case MAIL_CONNECTING: {
        if (tdeck_wifi_connected()) {
            delay(500); // let DHCP/DNS settle before the handshake
            s_state = MAIL_WORK;
        } else if ((int32_t)(millis() - s_deadline) > 0) {
            tdeck_wifi_disconnect_now();
            s_ownWifi = false;
            mailFail("wi-fi did not connect");
        }
        break;
    }
    case MAIL_WORK: {
        bool ok = imapCheck();
        if (s_ownWifi) {
            tdeck_wifi_disconnect_now();
            s_ownWifi = false;
        }
        if (ok)
            s_state = MAIL_DONE;
        else if (s_state != MAIL_ERROR)
            mailFail("mail check failed");
        break;
    }
    default:
        break;
    }
}

#else // !HAS_WIFI
extern "C" bool tdeck_mail_check(void) { return false; }
extern "C" bool tdeck_mail_connect_test(void) { return false; }
extern "C" int tdeck_mail_poll(void) { return -1; }
extern "C" void tdeck_mail_counts(int *t, int *u)
{
    if (t)
        *t = -1;
    if (u)
        *u = -1;
}
extern "C" const char *tdeck_mail_error(void) { return "no wi-fi on this board"; }
extern "C" void tdeck_mail_service(void) {}
#endif
