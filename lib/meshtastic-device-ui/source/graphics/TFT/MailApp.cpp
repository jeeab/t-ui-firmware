#include "graphics/common/SdCard.h"
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include "util/ILog.h"
#include <cstdio>
#include <cstring>

// -----------------------------------------------------------------------------------------
// MAIL - Gmail over IMAP. This file is the SETUP FORM and the status screen; the network side
// lives in src/TDeckMail.cpp.
//
// Jake, 2026-09-23: "Instead of the SD card needing a password, can it be a form that I type in
// on the device then it saves to the SD card?" - yes, and it is strictly better than the card
// route in two ways beyond convenience: there is no step where the password sits in a text file
// on a PC, and it means I never handle it at any point. It goes from his fingers to the card.
//
// ⛔ THE PASSWORD MUST NEVER LEAVE THIS DEVICE, AND THAT INCLUDES MY OWN TOOLS.
// The @@shot command streams the framebuffer over USB, so a screenshot taken while this form is
// filled in would put the password in a PNG on my machine and in the conversation. That is not
// a hypothetical - I have been screenshotting this device all session to verify work. So the
// form BLOCKS screenshots while it holds anything, rather than relying on me to remember.
// It is also never logged, and the field is cleared the moment it is saved.
// -----------------------------------------------------------------------------------------

extern "C" bool tdeck_mail_check(void);
extern "C" int tdeck_mail_poll(void);
extern "C" void tdeck_mail_counts(int *total, int *unseen);
extern "C" const char *tdeck_mail_error(void);
extern "C" void tdeck_mail_forget_creds(void); // re-read /gmail.txt after the form saves
extern "C" void tdeck_shot_block(bool on);     // refuse @@shot while a secret is on screen

static lv_obj_t *screen = nullptr;
static lv_obj_t *addrArea = nullptr;
static lv_obj_t *passArea = nullptr;
static lv_obj_t *statusLbl = nullptr;
static lv_obj_t *countLbl = nullptr;
static lv_obj_t *setupBox = nullptr;
static lv_obj_t *statusBox = nullptr;
static bool checking = false;

static bool haveCreds(void)
{
    FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
    if (!f)
        return false;
    char line[96];
    int n = f.fgets(line, sizeof(line));
    f.close();
    return n > 3; // an address, at minimum
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *txt, uint32_t colour, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void showSetup(bool on)
{
    if (setupBox)
        on ? lv_obj_clear_flag(setupBox, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(setupBox, LV_OBJ_FLAG_HIDDEN);
    if (statusBox)
        on ? lv_obj_add_flag(statusBox, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(statusBox, LV_OBJ_FLAG_HIDDEN);
    // ⛔ THE SCREENSHOT BLOCK IS NOT SET HERE. It is DERIVED from what is actually on screen,
    // every poll, in mail_service_ui(). Setting it at this transition is what the first version
    // did, and leaving the app by Back or Home does not pass through here - so the block stuck
    // ON and every screenshot for the rest of the session was refused, on every screen. Tested
    // and caught: "@@err shot refused - a password field is on screen" while sitting on the
    // launcher. Same shape as three other bugs today: a flag set on one path and cleared on
    // none of the others.
}

// ⭐ GOOGLE SHOWS THE APP PASSWORD AS "abcd efgh ijkl mnop" AND THE SPACES ARE NOT PART OF IT.
// Stripping here as well as in TDeckMail keeps the FILE clean, so anyone reading the card later
// sees the real thing rather than a version that only works because something strips it.
static void stripSpaces(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n')
            *w++ = *r;
    *w = 0;
}

static void onSave(lv_event_t *)
{
    char addr[96], pass[64];
    strncpy(addr, lv_textarea_get_text(addrArea), sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = 0;
    strncpy(pass, lv_textarea_get_text(passArea), sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = 0;
    stripSpaces(addr);
    stripSpaces(pass);

    if (!strchr(addr, '@')) {
        lv_label_set_text(statusLbl, "That does not look like an email address");
        return;
    }
    // ⛔ GMAIL ONLY, AND NOT ARBITRARILY. Jake asked whether this works for other providers.
    // It does not yet, and the reason is the security design rather than laziness: the IMAP
    // connection verifies the server against ONE embedded root certificate - Google's GTS Root
    // R1 - because a full CA bundle does not fit here. Outlook, Yahoo and iCloud chain to
    // different roots, so they would not fail with "wrong server", they would fail the
    // certificate check, which reads as a network fault and is miserable to diagnose.
    // Supporting them means embedding their roots too, and verifying each. Until then, say so
    // up front rather than letting someone type a Yahoo address and hit a baffling error.
    {
        const char *at = strchr(addr, '@');
        if (strcasecmp(at, "@gmail.com") && strcasecmp(at, "@googlemail.com")) {
            lv_label_set_text(statusLbl, "Gmail only for now - the security check is pinned to "
                                         "Google's certificate");
            return;
        }
    }
    if (strlen(pass) < 16) {
        // Google's app passwords are exactly 16 characters. Saying so beats a login failure
        // twenty seconds later that could mean anything.
        lv_label_set_text(statusLbl, "App passwords are 16 characters - check it");
        return;
    }

    FsFile f = SDFs.open("/gmail.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        lv_label_set_text(statusLbl, "Could not write to the SD card");
        return;
    }
    f.println(addr);
    f.println(pass);
    f.close();
    // ⛔ NOT LOGGED, and wiped from RAM and from the field immediately. The only copy that
    // should exist after this line is the one on the card.
    memset(pass, 0, sizeof(pass));
    lv_textarea_set_text(passArea, "");
    tdeck_mail_forget_creds(); // drop the cached copy so the new file is picked up
    LOG_INFO("[MAIL] credentials saved for %s", addr); // the ADDRESS only, never the password

    showSetup(false);
    lv_label_set_text(statusLbl, "Saved. Checking...");
    checking = tdeck_mail_check();
}

static void onCheck(lv_event_t *)
{
    if (checking)
        return;
    lv_label_set_text(statusLbl, "Checking...");
    checking = tdeck_mail_check();
    if (!checking)
        lv_label_set_text(statusLbl, "Busy - try again in a moment");
}

static void onEdit(lv_event_t *)
{
    lv_label_set_text(statusLbl, "");
    showSetup(true);
}

// Called from the UI poll while this screen is up.
extern "C" void mail_service_ui(void)
{
    // Derive the screenshot block from the CURRENT state rather than trusting a transition to
    // have set it. Cheap - two pointer compares - and it cannot be left stale by any exit path,
    // including ones that do not exist yet.
    {
        static bool blocked = false;
        const bool onMail = screen && lv_screen_active() == screen;
        const bool wantBlock = onMail && setupBox && !lv_obj_has_flag(setupBox, LV_OBJ_FLAG_HIDDEN);
        if (wantBlock != blocked) {
            blocked = wantBlock;
            tdeck_shot_block(wantBlock);
        }
    }
    if (!screen || lv_screen_active() != screen || !checking)
        return;
    int st = tdeck_mail_poll();
    if (st == 0)
        return;
    checking = false;
    if (st > 0) {
        int total = -1, unseen = -1;
        tdeck_mail_counts(&total, &unseen);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d unread", unseen < 0 ? 0 : unseen);
        lv_label_set_text(countLbl, buf);
        snprintf(buf, sizeof(buf), "%d messages in the inbox", total < 0 ? 0 : total);
        lv_label_set_text(statusLbl, buf);
    } else {
        lv_label_set_text(countLbl, "--");
        lv_label_set_text(statusLbl, tdeck_mail_error());
    }
}

extern "C" void mail_open(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        tui_statusbar_reserve(screen);

        // ---------- setup form ----------
        setupBox = lv_obj_create(screen);
        lv_obj_remove_style_all(setupBox);
        lv_obj_set_pos(setupBox, 0, 26);
        lv_obj_set_size(setupBox, 320, 214);
        lv_obj_clear_flag(setupBox, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *t1 = lv_label_create(setupBox);
        lv_label_set_text(t1, "Gmail address");
        // (Gmail specifically - see the certificate note in onSave.)
        lv_obj_set_style_text_color(t1, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_set_pos(t1, 6, 0);

        addrArea = lv_textarea_create(setupBox);
        lv_obj_set_pos(addrArea, 4, 18);
        lv_obj_set_size(addrArea, 312, 34);
        lv_textarea_set_one_line(addrArea, true);
        lv_textarea_set_max_length(addrArea, 80);
        lv_textarea_set_placeholder_text(addrArea, "you@gmail.com");
        // Jake: "could you make the form end in at @gmail.com so that people know". Prefilling
        // it says which provider this is for before anyone types a word - and the caret is put
        // at the START so the name goes in front of it rather than after the domain.
        lv_textarea_set_text(addrArea, "@gmail.com");
        lv_textarea_set_cursor_pos(addrArea, 0);
        lv_obj_set_style_bg_color(addrArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(addrArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(addrArea, 0, LV_PART_MAIN);
        // Solid caret: a blinking one only repaints when something else changes, so on this
        // device it reads as "there is no cursor". Same reason NotesApp and Gemini use one.
        lv_obj_set_style_bg_color(addrArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(addrArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(addrArea, 0, LV_PART_CURSOR);

        lv_obj_t *t2 = lv_label_create(setupBox);
        lv_label_set_text(t2, "App password (16 characters)");
        lv_obj_set_style_text_color(t2, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_set_pos(t2, 6, 58);

        passArea = lv_textarea_create(setupBox);
        lv_obj_set_pos(passArea, 4, 76);
        lv_obj_set_size(passArea, 312, 34);
        lv_textarea_set_one_line(passArea, true);
        lv_textarea_set_max_length(passArea, 40);
        lv_textarea_set_placeholder_text(passArea, "abcd efgh ijkl mnop");
        lv_obj_set_style_bg_color(passArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_text_color(passArea, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_border_width(passArea, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(passArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
        lv_obj_set_style_bg_opa(passArea, LV_OPA_50, LV_PART_CURSOR);
        lv_obj_set_style_anim_duration(passArea, 0, LV_PART_CURSOR);
        // ⚠️ DELIBERATELY NOT MASKED. It is a 16-character random string typed on a thumb
        // keyboard; hiding it guarantees typos that surface later as "login rejected" with no
        // way to tell a wrong character from a wrong setting. The screen is in your hand, and
        // the real exposure - a screenshot over the cable - is blocked instead.

        lv_obj_t *hint = lv_label_create(setupBox);
        lv_obj_set_pos(hint, 6, 116);
        lv_obj_set_width(hint, 308);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_label_set_text(hint, "Gmail only. Google Account > Security > App passwords - needs "
                                "2-Step Verification on. Spaces are fine, they get stripped.");

        lv_obj_t *saveBtn = makeButton(setupBox, "Save", 0x30d158, onSave);
        lv_obj_set_size(saveBtn, 150, 34);
        lv_obj_set_pos(saveBtn, 4, 172);
        lv_obj_t *cancelBtn = makeButton(setupBox, "Cancel", 0x3a3a3c, [](lv_event_t *) {
            lv_textarea_set_text(passArea, ""); // never leave it sitting on a screen nobody is at
            if (haveCreds())
                showSetup(false);
        });
        lv_obj_set_size(cancelBtn, 158, 34);
        lv_obj_set_pos(cancelBtn, 158, 172);

        // ---------- status screen ----------
        statusBox = lv_obj_create(screen);
        lv_obj_remove_style_all(statusBox);
        lv_obj_set_pos(statusBox, 0, 26);
        lv_obj_set_size(statusBox, 320, 214);
        lv_obj_clear_flag(statusBox, LV_OBJ_FLAG_SCROLLABLE);

        countLbl = lv_label_create(statusBox);
        lv_obj_set_style_text_color(countLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(countLbl, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_pos(countLbl, 8, 10);
        lv_label_set_text(countLbl, "--");

        lv_obj_t *chk = makeButton(statusBox, "Check now", 0x0a84ff, onCheck);
        lv_obj_set_size(chk, 150, 34);
        lv_obj_set_pos(chk, 4, 60);
        lv_obj_t *edit = makeButton(statusBox, "Account", 0x3a3a3c, onEdit);
        lv_obj_set_size(edit, 158, 34);
        lv_obj_set_pos(edit, 158, 60);

        statusLbl = lv_label_create(screen);
        lv_obj_set_pos(statusLbl, 6, 132);
        lv_obj_set_width(statusLbl, 308);
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_text(statusLbl, "");
    }

    // First run goes straight to the form; after that, straight to the inbox count.
    showSetup(!haveCreds());
    if (lv_group_get_default() && addrArea) {
        lv_group_add_obj(lv_group_get_default(), addrArea);
        lv_group_add_obj(lv_group_get_default(), passArea);
    }
    lv_screen_load(screen);
}
