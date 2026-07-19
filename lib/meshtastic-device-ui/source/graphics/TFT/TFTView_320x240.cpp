#if HAS_TFT && defined(VIEW_320x240) || defined(VIEW_240x320)

#include "graphics/view/TFT/TFTView_320x240.h"
#include "Arduino.h"
#include "graphics/common/BatteryLevel.h"
#include "graphics/common/LoRaPresets.h"
#include "graphics/common/Ringtones.h"
#include "graphics/common/ViewController.h"
#include "graphics/driver/DisplayDriver.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "graphics/map/MapPanel.h"
#include "graphics/map/TileProvider.h"
#include "graphics/map/URLService.h"
#include "graphics/view/TFT/Themes.h"
#include "images.h"
#include "input/InputDriver.h"
#include "lv_i18n.h"
#include "lvgl_private.h"
#include "styles.h"
#include "ui.h"
#include "util/FileLoader.h"
#include "util/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <list>
#include <locale>
#include <random>
#include <sstream>
#include <time.h>
#ifdef ARDUINO_ARCH_ESP32
#include <HTTPClient.h> // Maps: USGS region downloader (tile fetch over on-demand WiFi)
#include <WiFiClientSecure.h>
#endif

#if defined(ARCH_PORTDUINO)
#include "PortduinoFS.h"
fs::FS &fileSystem = PortduinoFS;
#else
#include "LittleFS.h"
fs::FS &fileSystem = LittleFS;
#endif

#if defined(ARCH_PORTDUINO)
#include "util/LinuxHelper.h"
// #include "graphics/map/LinuxFileSystemService.h"
#include "graphics/map/SDCardService.h"
#elif defined(HAS_SD_MMC)
#include "graphics/map/SDCardService.h"
#else
#include "graphics/map/SdFatService.h"
#endif
#include "graphics/common/SdCard.h"

#ifndef MAX_NUM_NODES_VIEW
#define MAX_NUM_NODES_VIEW 250
#endif

#ifndef PACKET_LOGS_MAX
#define PACKET_LOGS_MAX 200
#endif

LV_IMAGE_DECLARE(img_circle_image);
LV_IMAGE_DECLARE(img_no_tile_image);
LV_IMAGE_DECLARE(node_location_pin24_image);

#define CR_REPLACEMENT 0x0C              // dummy to record several lines in a one line textarea
#define THIS TFTView_320x240::instance() // need to use this in all static methods

// Mesh kill-switch bridge into the firmware (defined in src/TDeckMeshSwitch.cpp).
// Linked across the UI/mesh boundary as a free function (no shared header).
extern "C" void tdeck_set_mesh_enabled(bool on);
extern "C" bool tdeck_get_mesh_enabled(void);
// GPS status bridge (src/TDeckGpsBridge.cpp) — live sats-in-view / lock / fix from
// the GPS driver itself, available while acquiring, before any position packet.
extern "C" uint32_t tdeck_gps_num_sats(void);
extern "C" bool tdeck_gps_has_lock(void);
extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon);
extern "C" uint32_t tdeck_gps_dop(void); // PDOP x100, 0 = unknown
// GPS on/off + check-interval control bridge (src/TDeckGpsControl.cpp) — applied
// live with no reboot.
extern "C" void tdeck_gps_set_enabled(bool on);
extern "C" void tdeck_gps_set_interval(uint32_t secs);
extern "C" bool tdeck_gps_get_enabled(void);
extern "C" void tdeck_gps_kick(void); // re-arm the GPS search after a sleep
// Time zone (so the clock reads local, not UTC). Index into the list in TDeckTimeZone.cpp,
// which must stay in the same order as the dropdown built in createSettingsScreen().
extern "C" void tdeck_tz_set(int idx);
extern "C" int tdeck_tz_get(void);

// WiFi status bridge (src/TDeckWifi.cpp): 0=off/unconfigured, 1=connecting, 2=connected
extern "C" int tdeck_wifi_state(void);
extern "C" void tdeck_wifi_ip(char *buf, int len);
// WiFi network scan (for the pick-from-a-list chooser)
extern "C" void tdeck_wifi_scan_start(void);
extern "C" int tdeck_wifi_scan_done(void); // -2 failed, -1 running, >=0 count
extern "C" void tdeck_wifi_scan_get(int i, char *ssid, int len, int *rssi, int *secure);
extern "C" void tdeck_wifi_scan_free(void);
// On-demand WiFi (for the File Share screen — connect without a reboot, drop it after)
extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk);
extern "C" void tdeck_wifi_disconnect_now(void);
extern "C" bool tdeck_wifi_connected(void);
// FTP file-share engine (TDeckFtp.cpp)
extern "C" void tdeck_ftp_start(const char *user, const char *pass);
extern "C" void tdeck_ftp_stop(void);
extern "C" void tdeck_ftp_service(void);
extern "C" bool tdeck_ftp_running(void);
// Snake game module (SnakeGame.cpp) — opened from its launcher tile.
extern "C" void snake_open(void);
// Stopwatch module (StopwatchApp.cpp) — opened from its launcher tile.
extern "C" void stopwatch_open(void);
// Notes module (NotesApp.cpp) — .txt notes on the SD card.
extern "C" void notes_open(void);
extern "C" void notes_open_file(const char *path); // Files app opens .txt files with this
// Calculator module (CalculatorApp.cpp).
extern "C" void calculator_open(void);
extern "C" void lua_app_open(void);   // Lua app engine — hello demo (from SD)
extern "C" void breakout_open(void);  // Breakout game (Lua, from SD)
extern "C" void metronome_open(void);  // Metronome tool (Lua, from SD)
extern "C" void lua_seed_bundled(void); // first-run install of bundled SD apps (once, versioned)
extern "C" void lua_run_path(const char *path); // run a user app by its SD script path
// Live RAM figures from the firmware side (TDeckMemInfo.cpp), linked across the
// UI/firmware boundary — used by the launcher's memory readout.
extern "C" uint32_t tdeck_free_heap(void);
extern "C" uint32_t tdeck_min_free_heap(void);
extern "C" uint32_t tdeck_free_psram(void);
extern "C" uint32_t tdeck_min_free_psram(void);
extern "C" int tdeck_lua_selftest(void); // Stage-1 app-engine proof: runs a Lua script on-device
// Crash diagnostics (TDeckMemInfo.cpp): why the device last restarted + last
// session's worst memory lows, persisted across the reboot in NVS.
extern "C" void tdeck_diag_boot(void);
extern "C" void tdeck_diag_tick(void);
extern "C" const char *tdeck_prev_reason_str(void);
extern "C" bool tdeck_prev_reason_bad(void);
extern "C" uint32_t tdeck_prev_psram_low(void);
extern "C" uint32_t tdeck_prev_heap_low(void);
// Freeze detector: if the previous session's main loop stalled before the reboot,
// how long it was stuck and which OSThread it was stuck inside.
extern "C" uint32_t tdeck_prev_stall_ms(void);
extern "C" const char *tdeck_prev_stall_thread(void);
extern "C" const char *tdeck_prev_stall_lock(void); // spiLock holder + loop state at stall time
void playBeep(); // buzz.cpp (C++ linkage) — completion chirp for the map downloader
// Sound toggle (TDeckBeep.cpp): drives Meshtastic's buzzer_mode — one switch for game/timer
// beeps AND message-notification sounds. Persists in the device config (no reboot).
extern "C" bool tdeck_sound_get_enabled(void);
extern "C" void tdeck_sound_set_enabled(bool on);
#if defined(INPUTDRIVER_ENCODER_TYPE)
// Set by the trackball driver on a double-click; polled by the launcher to go Home.
extern volatile bool tb_home_request;
#endif

// Lock/wake gating, shared with the display + input drivers (declared extern in LGFXDriver.h,
// EncoderInputDriver.cpp, I2CKeyboardInputDriver.cpp). Defined here.
//  tdeck_input_gated -> screen is dark: swallow touch/keyboard/trackball-roll so only a
//                       trackball double-click can wake it.
//  tdeck_hold_dark   -> manual lock: LGFXDriver snaps the backlight to black and holds it.
volatile bool tdeck_input_gated = false;
volatile bool tdeck_hold_dark = false;
volatile bool tdeck_prog_mode = false;     // BT programming-mode screen up: suppress dim/gate/lock
volatile bool tdeck_prog_key_exit = false; // a keypress in programming mode requests exit (set by kbd driver)
//  tdeck_wake_request -> a physical keyboard key was pressed while the screen was dark/gated.
//  Polled alongside the trackball double-click so the keyboard is an alternate, more reliable
//  wake (the trackball button is stiff in Jake's 3D case). Wakes + shows the PIN pad, same path.
volatile bool tdeck_wake_request = false;
//  tdeck_calib_request -> Alt+C was pressed (keyboard emits 0x0C). Polled by the launcher to run
//  touch-screen calibration from the lock pad — a touch-independent fix when the screen is
//  miscalibrated (set by the keyboard driver, see I2CKeyboardInputDriver.cpp).
volatile bool tdeck_calib_request = false;

#define LV_COLOR_HEX(C)                                                                                                          \
    {                                                                                                                            \
        .blue = (C >> 0) & 0xff, .green = (C >> 8) & 0xff, .red = (C >> 16) & 0xff                                               \
    }

#define VALID_TIME(T) (T > 1000000 && T < UINT32_MAX)

constexpr lv_color_t colorRed = LV_COLOR_HEX(0xff5555);
constexpr lv_color_t colorDarkRed = LV_COLOR_HEX(0xa70a0a);
constexpr lv_color_t colorOrange = LV_COLOR_HEX(0xff8c04);
constexpr lv_color_t colorYellow = LV_COLOR_HEX(0xdbd251);
constexpr lv_color_t colorBlueGreen = LV_COLOR_HEX(0x05f6cb);
constexpr lv_color_t colorBlue = LV_COLOR_HEX(0x436C70);
constexpr lv_color_t colorGray = LV_COLOR_HEX(0x757575);
constexpr lv_color_t colorLightGray = LV_COLOR_HEX(0xAAFBFF);
constexpr lv_color_t colorMidGray = LV_COLOR_HEX(0x808080);
constexpr lv_color_t colorDarkGray = LV_COLOR_HEX(0x303030);
constexpr lv_color_t colorMesh = LV_COLOR_HEX(0x67ea94);

// children index of nodepanel lv objects (see addNode)
enum NodePanelIdx {
    node_img_idx,
    node_btn_idx,
    node_lbl_idx,
    node_lbs_idx,
    node_bat_idx,
    node_lh_idx,
    node_sig_idx,
    node_pos1_idx,
    node_pos2_idx,
    node_tm1_idx,
    node_tm2_idx
};

enum ScrollDirection {
    scrollDownLeft = 1,
    scrollDown = 2,
    scrollDownRight = 3,
    scrollLeft = 4,
    scrollRight = 6,
    scrollUpLeft = 7,
    scrollUp = 8,
    scrollUpRight = 9,
};

extern const char *firmware_version;

// Our launcher's own version, shown at the bottom of Settings. Bump this on every release and
// keep it in step with t-ui-installer/manifest.json, so "what's on the device?" has an answer.
#define TUI_VERSION "2026.07.18.7"

TFTView_320x240 *TFTView_320x240::gui = nullptr;
lv_obj_t *TFTView_320x240::currentPanel = nullptr;
lv_obj_t *TFTView_320x240::spinnerButton = nullptr;
uint32_t TFTView_320x240::currentNode = 0;
time_t TFTView_320x240::startTime = 0;
uint32_t TFTView_320x240::pinKeys = 0;
bool TFTView_320x240::screenLocked = false;
bool TFTView_320x240::screenUnlockRequest = false;

TFTView_320x240 *TFTView_320x240::instance(void)
{
    if (!gui) {
        gui = new TFTView_320x240(nullptr, DisplayDriverFactory::create(320, 240));
    }
    return gui;
}

TFTView_320x240 *TFTView_320x240::instance(const DisplayDriverConfig &cfg)
{
    if (!gui) {
        gui = new TFTView_320x240(&cfg, DisplayDriverFactory::create(cfg));
    }
    return gui;
}

TFTView_320x240::TFTView_320x240(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : MeshtasticView(cfg, driver, new ViewController), screensInitialised(false), nodesFiltered(0), nodesChanged(true),
      processingFilter(false), packetLogEnabled(false), detectorRunning(false), cardDetected(false), formatSD(false),
      packetCounter(0), actTime(0), uptime(0), lastHeard(0), hasPosition(false), myLatitude(0), myLongitude(0),
      topNodeLL(nullptr), scans(0), selectedHops(0), chooseNodeSignalScanner(false), chooseNodeTraceRoute(false), qr(nullptr),
      db{}
{
    filter.active = false;
    highlight.active = false;
    objects.main_screen = nullptr;
}

/**
 * @brief Initialize view and boot screen
 * Note: We'll wait until we got our persistent data and then initialize
 *       the remaining screens.
 */
void TFTView_320x240::init(IClientBase *client)
{
    ILOG_DEBUG("TFTView_320x240 init...");
    ILOG_DEBUG("TFTView_320x240 db size: %d", sizeof(TFTView_320x240));
    ILOG_DEBUG("### Images size in flash ###");
    uint32_t total_size = 0;
    for (int i = 0; i < sizeof(images) / sizeof(ext_img_desc_t); i++) {
        total_size += images[i].img_dsc->data_size;
        ILOG_DEBUG("    %s: %d", images[i].name, images[i].img_dsc->data_size);
    }
    ILOG_DEBUG("================================");
    ILOG_DEBUG("### Total size: %d bytes ###", total_size);

    MeshtasticView::init(client);

    ui_init_boot();
    FileLoader::init(&fileSystem);
    if (!FileLoader::loadBootImage(objects.boot_logo))
        lv_image_set_src(objects.boot_logo, &img_meshtastic_boot_logo_image);
    // if boot logo is too big remove the label and center the image
    lv_obj_update_layout(objects.boot_logo);
    if (lv_obj_get_height(objects.boot_logo) > lv_display_get_vertical_resolution(displaydriver->getDisplay()) / 2) {
        lv_obj_set_pos(objects.boot_logo, 0, 0);
        lv_obj_add_flag(objects.firmware_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(objects.firmware_label, "Booting...");
    }

    // ---- T-UI splash restyle (cosmetic only — the boot->launcher handoff logic is untouched) ----
    // Black screen, big "T-UI" title, "booting" below. The stock Meshtastic logo/URL are hidden,
    // not removed, so the reboot spinner and boot-message plumbing keep working.
    lv_obj_set_style_bg_color(objects.boot_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_add_flag(objects.boot_logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.meshtastic_url, LV_OBJ_FLAG_HIDDEN);
    splash_title = lv_label_create(objects.boot_screen);
    lv_label_set_text(splash_title, "T-UI");
    lv_obj_set_style_text_color(splash_title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(splash_title, &ui_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(splash_title, 8, LV_PART_MAIN);
    lv_obj_align(splash_title, LV_ALIGN_CENTER, 0, -18);
    lv_obj_clear_flag(objects.firmware_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(objects.firmware_label, "booting");
    lv_obj_set_style_text_color(objects.firmware_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(objects.firmware_label, LV_ALIGN_CENTER, 0, 14);
    // reboot spinner arc: recolor for the black background
    lv_obj_set_style_arc_color(objects.boot_logo_arc, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(objects.boot_logo_arc, lv_color_hex(0x2c2c2e), LV_PART_MAIN);

    time(&lastrun60);
    time(&lastrun10);
    time(&lastrun5);
    time(&lastrun1);

    lv_obj_add_event_cb(objects.boot_logo_button, ui_event_LogoButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.blank_screen_button, ui_event_BlankScreenButton, LV_EVENT_ALL, NULL);

    lv_timer_create(timer_event_programming_mode, 3000, NULL); // timer for programming mode button active
}

/**
 * @brief initialize UI with persistent data
 */
bool TFTView_320x240::setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig)
{
    if (uiconfig.version == 1) {
        ILOG_INFO("setupUIConfig version %d", uiconfig.version);
        db.uiConfig = uiconfig;
        if (db.uiConfig.screen_timeout == 1) {
            db.uiConfig.screen_timeout = 30;
            controller->storeUIConfig(db.uiConfig);
        }
    } else {
        ILOG_WARN("invalid uiconfig version %d, reset UI settings to default", uiconfig.version);
        db.uiConfig.version = 1;
        db.uiConfig.screen_brightness = 153;
        db.uiConfig.screen_timeout = 30;
        controller->storeUIConfig(db.uiConfig);
    }

    lv_i18n_init(lv_i18n_language_pack);
    setLocale(db.uiConfig.language);

    if (state == MeshtasticView::eEnterProgrammingMode || state == MeshtasticView::eProgrammingMode ||
        state == MeshtasticView::eWaitingForReboot) {
        enterProgrammingMode();
        return false;
    }

    state = MeshtasticView::eSetupUIConfig;

    // now we have set language, continue creating all screens
    if (!screensInitialised)
        init_screens();

    // set language
    setLanguage(db.uiConfig.language);

    // TODO: set virtual keyboard according language
    //  setKeyboard(db.uiConfig.language);

    // set theme
    setTheme(db.uiConfig.theme);

    // grey out bell until we got the ringtone (0 = silent)
    Themes::recolorButton(objects.home_bell_button, false);
    Themes::recolorText(objects.home_bell_label, false);

    lv_obj_set_style_bg_img_recolor(objects.home_button, colorMesh, LV_PART_MAIN | LV_STATE_DEFAULT);

    // set brightness
    if (displaydriver->hasLight())
        THIS->setBrightness(db.uiConfig.screen_brightness);

    // set timeout
    THIS->setTimeout(db.uiConfig.screen_timeout);

    // set screen/settings lock
    char buf[40];
    lv_snprintf(buf, 40, _("Lock: %s/%s"), db.uiConfig.screen_lock ? _("on") : _("off"),
                db.uiConfig.settings_lock ? _("on") : _("off"));
    lv_label_set_text(objects.basic_settings_screen_lock_label, buf);

    // set node filter options
    meshtastic_NodeFilter &filter = db.uiConfig.node_filter;
    lv_obj_set_state(objects.nodes_filter_unknown_switch, LV_STATE_CHECKED, filter.unknown_switch);
    lv_obj_set_state(objects.nodes_filter_offline_switch, LV_STATE_CHECKED, filter.offline_switch);
    lv_obj_set_state(objects.nodes_filter_public_key_switch, LV_STATE_CHECKED, filter.public_key_switch);
    // lv_dropdown_set_selected(objects.nodes_filter_channel_dropdown, filter.channel);
    lv_dropdown_set_selected(objects.nodes_filter_hops_dropdown, filter.hops_away);
    // lv_obj_set_state(objects.nodes_filter_mqtt_switch, LV_STATE_CHECKED, filter.mqtt_switch);
    lv_obj_set_state(objects.nodes_filter_position_switch, LV_STATE_CHECKED, filter.position_switch);
    lv_textarea_set_text(objects.nodes_filter_name_area, filter.node_name);

    // set node highlight options
    meshtastic_NodeHighlight &highlight = db.uiConfig.node_highlight;
    lv_obj_set_state(objects.nodes_hl_active_chat_switch, LV_STATE_CHECKED, highlight.chat_switch);
    lv_obj_set_state(objects.nodes_hl_position_switch, LV_STATE_CHECKED, highlight.position_switch);
    lv_obj_set_state(objects.nodes_hl_telemetry_switch, LV_STATE_CHECKED, highlight.telemetry_switch);
    lv_obj_set_state(objects.nodes_hliaq_switch, LV_STATE_CHECKED, highlight.iaq_switch);
    lv_textarea_set_text(objects.nodes_hl_name_area, highlight.node_name);

    // initialize own node panel
    if (ownNode && objects.node_panel)
        nodes[ownNode] = objects.node_panel;

    // touch screen calibration data
    uint16_t *parameters = (uint16_t *)db.uiConfig.calibration_data.bytes;
    if (db.uiConfig.calibration_data.size == 16 && (parameters[0] || parameters[7])) {
#ifndef IGNORE_CALIBRATION_DATA
        bool done = displaydriver->calibrate(parameters);
        char buf[32];
        lv_snprintf(buf, sizeof(buf), _("Screen Calibration: %s"), done ? _("done") : _("default"));
        lv_label_set_text(objects.basic_settings_calibration_label, buf);
#endif
    }

    // update home panel bell text
    setBellText(db.uiConfig.alert_enabled, !db.silent);
    bool off = !db.uiConfig.alert_enabled && db.silent;
    Themes::recolorButton(objects.home_bell_button, !off);
    Themes::recolorText(objects.home_bell_label, !off);
    objects.home_bell_button->user_data = (void *)off;

    // check SD card
    updateSDCard();

    // function callback for the map panel node symbol
    drawObjectCB = [this](uint32_t id, uint16_t x, uint16_t y, uint8_t zoom) {
        auto img = nodeObjects[id];
        if (!x && !y && !zoom) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_move_foreground(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
        if (zoom >= 10 || (zoom >= 7 && nodeObjects.size() < 10)) {
            lv_obj_clear_flag(img->spec_attr->children[0], LV_OBJ_FLAG_HIDDEN);
        } else {
            // hide text
            lv_obj_add_flag(img->spec_attr->children[0], LV_OBJ_FLAG_HIDDEN);
        }
        if (zoom >= 4) {
            // pin location image
            lv_img_set_src(img, &img_node_location_pin24_image);
            lv_img_set_zoom(img, 256);
            lv_obj_set_pos(img, x - 20, y - 24); // img has 40x35 size, needle at 24
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_MID);
            // lv_obj_set_style_align(img->spec_attr->children[0], LV_ALIGN_BOTTOM_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            // circle image
            lv_img_set_src(img, &img_circle_image);
            lv_img_set_zoom(img, (zoom - 1) * 50 + 80);
            lv_obj_set_pos(img, x - 20, y - 17); // img has 40x35 size, circle at center
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
            // lv_obj_set_style_align(img->spec_attr->children[0], LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    };

    lv_disp_trig_activity(NULL);
    return true;
}

/**
 * @brief display custom message on boot screen
 *        Note: currently, the firmware version field is used and set in main()/setup()
 */
void TFTView_320x240::updateBootMessage(const char *msg)
{
    if (msg)
        lv_label_set_text(objects.firmware_label, msg);
}

// =====================================================================================
//  Custom iPhone-style app-grid launcher
//  Boots first; each tile loads MUI's main_screen and activates the matching panel.
//  To add an app: add one entry to kApps[] below. (name, icon, button, panel, topPanel)
// =====================================================================================
namespace {
struct LauncherApp {
    const char *name;
    const void *icon;      // reuse MUI's existing button images
    uint32_t color;        // per-app icon tint (iOS-style)
    lv_obj_t **button;     // &objects.* addresses are stable after ui_init()
    lv_obj_t **panel;
    lv_obj_t **topPanel;
    void (*action)();      // optional custom open action; if set, overrides panel logic
    const char *sdPath;    // if set, this is an SD-card app; its tile hides when the file is
                           // deleted (uninstall). nullptr = built-in firmware app.
};

static const LauncherApp kApps[] = {
    // Mesh opens the full Meshtastic MUI (nodes/map/messages/settings all live in there).
    {"Mesh", &img_home_button_image, 0x30d158, &objects.home_button, &objects.home_panel, &objects.top_panel, nullptr},
    // Notes = self-contained module (NotesApp.cpp), .txt files in /notes on SD.
    {"Notes", &img_messages_button_image, 0xffd60a, nullptr, nullptr, nullptr, &notes_open},
    // Calculator = self-contained module (CalculatorApp.cpp).
    {"Calculator", &img_nodes_button_image, 0xff9f0a, nullptr, nullptr, nullptr, &calculator_open},
    // Breakout = a Lua GAME loaded from /apps/breakout/main.lua on the SD card.
    {"Breakout", &img_nodes_button_image, 0x0a84ff, nullptr, nullptr, nullptr, &breakout_open, "/apps/breakout/main.lua"},
    // Metronome = a Lua TOOL loaded from /apps/metronome/main.lua on the SD card.
    // (Deleting its folder on the SD card uninstalls it — its tile then disappears on reboot.)
    {"Metronome", &img_messages_button_image, 0xbf5af2, nullptr, nullptr, nullptr, &metronome_open,
     "/apps/metronome/main.lua"},
    // Files = our own SD-card browser screen.
    {"Files", &img_map_button_image, 0x0a84ff, nullptr, nullptr, nullptr, &TFTView_320x240::openFilesAction},
    // Maps = our own standalone map app (same SD-card tiles as the mesh map, own screen).
    {"Maps", &img_map_button_image, 0x5ac8fa, nullptr, nullptr, nullptr, &TFTView_320x240::openMapsAction},
    // Settings = our own screen (mesh kill switch lives here), opened via a custom action.
    {"Settings", &img_settings_button_image, 0x8e8e93, nullptr, nullptr, nullptr, &TFTView_320x240::openSettingsAction},
    // Get Apps = browse the published catalog over Wi-Fi and install straight to the SD card.
    {"Get Apps", &img_nodes_button_image, 0xbf5af2, nullptr, nullptr, nullptr, &TFTView_320x240::openGetAppsAction},
    // Snake = self-contained game module (SnakeGame.cpp), WASD/swipe controls.
    {"Snake", &img_nodes_button_image, 0x30d158, nullptr, nullptr, nullptr, &snake_open},
    // Flashlight = full-white screen at max backlight (its own action).
    {"Flashlight", &img_nodes_button_image, 0xffd60a, nullptr, nullptr, nullptr, &TFTView_320x240::openFlashlightAction},
    // Stopwatch = self-contained count-up timer module (StopwatchApp.cpp).
    {"Stopwatch", &img_messages_button_image, 0x64d2ff, nullptr, nullptr, nullptr, &stopwatch_open},
};

// --- simple per-app icons, drawn from lv_obj primitives (no image assets needed) ---
lv_obj_t *icBox(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}
lv_obj_t *icRing(lv_obj_t *p, int x, int y, int d, uint32_t color, int borderW)
{
    lv_obj_t *o = icBox(p, x, y, d, d, color, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(o, borderW, LV_PART_MAIN);
    return o;
}
void buildTileIcon(lv_obj_t *tile, const char *name, uint32_t color)
{
    lv_obj_t *ic = lv_obj_create(tile); // transparent 46x40 icon canvas near the tile top
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 46, 40);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);

    if (!strcmp(name, "Snake")) { // green body squares + red food
        icBox(ic, 5, 22, 9, 9, 0x30d158, 2);
        icBox(ic, 14, 22, 9, 9, 0x30d158, 2);
        icBox(ic, 14, 13, 9, 9, 0x30d158, 2);
        icBox(ic, 23, 13, 9, 9, 0x30d158, 2);
        icBox(ic, 34, 4, 8, 8, 0xff453a, 4);
    } else if (!strcmp(name, "Flashlight")) { // bulb + rays
        icBox(ic, 17, 17, 12, 15, 0xffd60a, 4);
        icBox(ic, 19, 13, 8, 5, 0xfff3b0, 2);
        icBox(ic, 21, 2, 4, 8, 0xffd60a, 2);
        icBox(ic, 7, 7, 9, 4, 0xffd60a, 2);
        icBox(ic, 30, 7, 9, 4, 0xffd60a, 2);
    } else if (!strcmp(name, "Pinball")) { // bumper ring + ball + two angled flippers
        icRing(ic, 15, 1, 16, 0xff2d55, 3);
        icBox(ic, 20, 19, 7, 7, 0xffffff, LV_RADIUS_CIRCLE);
        lv_obj_t *lf = icBox(ic, 5, 31, 17, 5, 0xff9f0a, 2);
        lv_obj_set_style_transform_pivot_x(lf, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(lf, 2, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(lf, 200, LV_PART_MAIN); // 20 deg, tip slopes to center
        lv_obj_t *rf = icBox(ic, 25, 31, 17, 5, 0xff9f0a, 2);
        lv_obj_set_style_transform_pivot_x(rf, 17, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(rf, 2, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(rf, -200, LV_PART_MAIN);
    } else if (!strcmp(name, "Stopwatch")) { // ring + top button + hand
        icRing(ic, 10, 8, 26, color, 3);
        icBox(ic, 20, 2, 6, 6, color, 1);
        icBox(ic, 22, 15, 2, 9, color, 1);
    } else if (!strcmp(name, "Files")) { // folder
        icBox(ic, 8, 7, 15, 5, color, 1);
        icBox(ic, 6, 11, 34, 24, color, 3);
    } else if (!strcmp(name, "Notes")) { // page with lines
        icBox(ic, 11, 3, 24, 34, 0xf2f2f2, 3);
        icBox(ic, 15, 10, 16, 2, 0x8e8e93, 1);
        icBox(ic, 15, 16, 16, 2, 0x8e8e93, 1);
        icBox(ic, 15, 22, 16, 2, 0x8e8e93, 1);
        icBox(ic, 15, 28, 10, 2, 0x8e8e93, 1);
    } else if (!strcmp(name, "Calculator")) { // screen + button grid
        icBox(ic, 12, 3, 22, 34, 0x3a3a3c, 4);
        icBox(ic, 15, 6, 16, 6, color, 1);
        icBox(ic, 16, 18, 4, 4, 0xffffff, 1);
        icBox(ic, 22, 18, 4, 4, 0xffffff, 1);
        icBox(ic, 28, 18, 4, 4, 0xffffff, 1);
        icBox(ic, 16, 25, 4, 4, 0xffffff, 1);
        icBox(ic, 22, 25, 4, 4, 0xffffff, 1);
        icBox(ic, 28, 25, 4, 4, 0xffffff, 1);
    } else if (!strcmp(name, "Settings")) { // gear = ring + 4 nubs
        icRing(ic, 11, 7, 26, color, 5);
        icBox(ic, 21, 0, 4, 6, color, 1);
        icBox(ic, 21, 34, 4, 6, color, 1);
        icBox(ic, 1, 18, 6, 4, color, 1);
        icBox(ic, 39, 18, 6, 4, color, 1);
    } else if (!strcmp(name, "Maps")) { // location pin on a patch of "map"
        icBox(ic, 4, 6, 38, 28, 0x2f4f34, 4);          // map background
        icBox(ic, 8, 22, 16, 3, 0x6b8e6b, 1);          // a faint road
        icBox(ic, 18, 8, 14, 14, 0xff453a, LV_RADIUS_CIRCLE); // pin head
        icBox(ic, 22, 12, 6, 6, 0xffffff, LV_RADIUS_CIRCLE);  // pin hole
        icBox(ic, 23, 20, 4, 8, 0xff453a, 1);          // pin point
    } else if (!strcmp(name, "Mesh")) { // three connected nodes
        icBox(ic, 15, 2, 9, 9, color, LV_RADIUS_CIRCLE);
        icBox(ic, 4, 26, 9, 9, color, LV_RADIUS_CIRCLE);
        icBox(ic, 33, 26, 9, 9, color, LV_RADIUS_CIRCLE);
    } else if (!strcmp(name, "Breakout")) { // brick rows + ball + paddle
        icBox(ic, 5, 3, 10, 4, 0xff453a, 1);
        icBox(ic, 17, 3, 10, 4, 0xff9f0a, 1);
        icBox(ic, 29, 3, 10, 4, 0xffd60a, 1);
        icBox(ic, 5, 9, 10, 4, 0x30d158, 1);
        icBox(ic, 17, 9, 10, 4, 0xff453a, 1);
        icBox(ic, 29, 9, 10, 4, 0xff9f0a, 1);
        icBox(ic, 21, 22, 5, 5, 0xffffff, 2);      // ball
        icBox(ic, 14, 33, 18, 4, 0x0a84ff, 2);     // paddle
    } else if (!strcmp(name, "Metronome")) { // pyramid body + swinging bob
        icBox(ic, 12, 31, 22, 5, color, 1);
        icBox(ic, 15, 23, 16, 8, color, 1);
        icBox(ic, 18, 15, 10, 8, color, 1);
        icBox(ic, 21, 6, 5, 9, color, 1);
        icBox(ic, 26, 13, 6, 6, 0xffffff, 2);      // the bob, off to one side
    } else if (!strcmp(name, "Dice")) { // white die showing 5, red die showing 3
        icBox(ic, 3, 2, 20, 20, 0xf2f2f2, 5);
        icBox(ic, 6, 5, 4, 4, 0x1c1c1e, LV_RADIUS_CIRCLE);
        icBox(ic, 16, 5, 4, 4, 0x1c1c1e, LV_RADIUS_CIRCLE);
        icBox(ic, 11, 10, 4, 4, 0x1c1c1e, LV_RADIUS_CIRCLE);
        icBox(ic, 6, 15, 4, 4, 0x1c1c1e, LV_RADIUS_CIRCLE);
        icBox(ic, 16, 15, 4, 4, 0x1c1c1e, LV_RADIUS_CIRCLE);
        icBox(ic, 23, 17, 20, 20, 0xff453a, 5);
        icBox(ic, 26, 20, 4, 4, 0xffffff, LV_RADIUS_CIRCLE);
        icBox(ic, 31, 25, 4, 4, 0xffffff, LV_RADIUS_CIRCLE);
        icBox(ic, 36, 30, 4, 4, 0xffffff, LV_RADIUS_CIRCLE);
    } else if (!strcmp(name, "Reaction")) { // lightning bolt (two slanted strokes)
        lv_obj_t *b1 = icBox(ic, 20, 1, 8, 19, 0xffd60a, 2);
        lv_obj_set_style_transform_pivot_x(b1, 4, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(b1, 9, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(b1, 250, LV_PART_MAIN); // 25 deg lean
        lv_obj_t *b2 = icBox(ic, 15, 20, 8, 19, 0xffd60a, 2);
        lv_obj_set_style_transform_pivot_x(b2, 4, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(b2, 9, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(b2, 250, LV_PART_MAIN);
    } else { // fallback: a colored rounded square
        icBox(ic, 11, 8, 24, 24, color, 6);
    }
}
} // namespace

/**
 * @brief Build the custom launcher home screen (grid of app tiles from kApps[]).
 *        Must be called after ui_init() so objects.* screens/panels exist.
 */
void TFTView_320x240::createLauncher(void)
{
    ILOG_DEBUG("createLauncher()");

    launcher_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(launcher_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(launcher_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ---- top status bar (~24px): mesh dot + label left, clock center, battery right ----
    // The centre label shows the time (from the GPS satellites, in the zone set in Settings).
    // Tap it for the full diagnostics popup — that's where the RAM figures live now; it used
    // to show "ram NNk/NNk" here permanently, which was crash-hunting scaffolding.
    // It still flips to a red "last: <reason>" for ~20s after a fault restart.
    tdeck_diag_boot();   // capture last restart reason + last session's memory lows (once)
    lua_seed_bundled();  // install bundled SD apps on first run of this firmware (once, then user-owned)
    launcher_mem_label = lv_label_create(launcher_screen);
    lv_label_set_text(launcher_mem_label, "T-Deck");
    lv_obj_set_style_text_color(launcher_mem_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(launcher_mem_label, &ui_font_montserrat_12, LV_PART_MAIN);
    // Offset right of center: leaves room on the left for the unread-message count near "mesh".
    lv_obj_align(launcher_mem_label, LV_ALIGN_TOP_MID, 40, 6);
    // Tap the readout for a full, readable diagnostics popup (the top-bar text is
    // cramped; this is the "hard to see the numbers" fix).
    lv_obj_add_flag(launcher_mem_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(launcher_mem_label, 12);
    lv_obj_add_event_cb(
        launcher_mem_label, [](lv_event_t *) { THIS->showDiagnostics(); }, LV_EVENT_CLICKED, NULL);
    lv_timer_create(
        [](lv_timer_t *) {
            if (!THIS->launcher_mem_label)
                return;
            tdeck_diag_tick();      // record new memory low-water marks (persists worst to NVS)
            THIS->logDiagBoot();    // once SD is up, append a line if we restarted from a fault
            char buf[64];
            // For the first ~20s after a fault, flash WHY we last restarted (red) so
            // it can't be missed; then settle into the live heap/PSRAM readout.
            if (tdeck_prev_reason_bad() && lv_tick_get() < 20000) {
                if (tdeck_prev_stall_ms()) // freeze detector caught it in the act: name the culprit
                    snprintf(buf, sizeof(buf), "last: %s in %s (%lus)", tdeck_prev_reason_str(),
                             tdeck_prev_stall_thread(), (unsigned long)(tdeck_prev_stall_ms() / 1000));
                else
                    snprintf(buf, sizeof(buf), "last: %s  ps low %luk", tdeck_prev_reason_str(),
                             (unsigned long)(tdeck_prev_psram_low() / 1024));
                lv_obj_set_style_text_color(THIS->launcher_mem_label, lv_color_hex(0xff453a), LV_PART_MAIN);
            } else {
                // Normal case: the clock. The device gets accurate time from the GPS satellites,
                // and the time zone set in Settings makes localtime() local. Before any valid time
                // arrives (or with no GPS fix yet) fall back to the device name rather than
                // showing a confidently wrong 00:00. The RAM figures this replaced were a
                // leftover from the crash hunt and still live in the tap-to-open diagnostics.
                time_t now;
                time(&now);
                if (VALID_TIME(now)) {
                    tm *lt = localtime(&now);
                    strftime(buf, sizeof(buf), "%I:%M %p", lt);
                    if (buf[0] == '0') // "07:05 PM" -> "7:05 PM"
                        memmove(buf, buf + 1, strlen(buf));
                } else {
                    strcpy(buf, "T-Deck");
                }
                lv_obj_set_style_text_color(THIS->launcher_mem_label, lv_color_hex(0xffffff), LV_PART_MAIN);
            }
            lv_label_set_text(THIS->launcher_mem_label, buf);
        },
        1000, NULL);

    // mesh on/off dot (reads meshEnabled; updated by setMeshEnabled()).
    mesh_status_icon = lv_obj_create(launcher_screen);
    lv_obj_remove_style_all(mesh_status_icon);
    lv_obj_set_size(mesh_status_icon, 12, 12);
    lv_obj_set_style_radius(mesh_status_icon, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mesh_status_icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mesh_status_icon, lv_color_hex(meshEnabled ? 0x30d158 : 0x8e8e93), LV_PART_MAIN);
    lv_obj_align(mesh_status_icon, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_clear_flag(mesh_status_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mstatus = lv_label_create(launcher_screen);
    lv_label_set_text(mstatus, "mesh");
    lv_obj_set_style_text_color(mstatus, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align_to(mstatus, mesh_status_icon, LV_ALIGN_OUT_RIGHT_MID, 5, 0);

    // Unread-message count, sitting just right of "mesh". Green so a new message stands out; blank
    // when there are none. Fed by updateUnreadMessages() (incremented on arrival, cleared to 0 the
    // moment the messages are opened — so checking your messages clears this count).
    launcher_unread_label = lv_label_create(launcher_screen);
    lv_obj_set_style_text_color(launcher_unread_label, lv_color_hex(0x30d158), LV_PART_MAIN);
    lv_obj_set_style_text_font(launcher_unread_label, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align_to(launcher_unread_label, mesh_status_icon, LV_ALIGN_OUT_RIGHT_MID, 50, 0);
    lv_label_set_text(launcher_unread_label, "");
    updateUnreadMessages(); // reflect any count that already accrued before the grid was built

    // battery percentage (top-right); fed by updateMetrics()->updateLauncherBattery().
    launcher_battery_label = lv_label_create(launcher_screen);
    lv_obj_set_style_text_color(launcher_battery_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(launcher_battery_label, LV_ALIGN_TOP_RIGHT, -8, 6);
    updateLauncherBattery();

    // ---- paged app grid (built into buildAppGrid so it can be rebuilt live) ----
    scanUserApps();  // discover any user apps on the SD card (once per boot)
    buildAppGrid();  // assemble the ordered list + build the tiles

    // NOTE: no on-screen "return to launcher" button. Home is reached exclusively by
    // trackball double-click (see the poll timer below + EncoderInputDriver). A focusable
    // button on main_screen would otherwise catch a normal single-click ENTER and jump
    // home unexpectedly while navigating the Meshtastic screens.

    // A config-sync event briefly loads main_screen right after boot. Re-assert the right
    // screen for the first few seconds so the device settles, then the timer self-stops.
    lv_timer_t *bootHome = lv_timer_create(
        [](lv_timer_t *) {
            // If we booted locked, keep the PIN pad up (don't let config-sync reveal the UI);
            // otherwise settle on the launcher grid.
            lv_obj_t *want = (THIS->lockState != LOCK_NONE && THIS->lockpad_screen) ? THIS->lockpad_screen
                                                                                    : THIS->launcher_screen;
            if (want && lv_screen_active() != want)
                lv_screen_load_anim(want, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        },
        400, NULL);
    lv_timer_set_repeat_count(bootHome, 15); // ~6s of boot settling (the config-sync can load
                                             // main_screen a few seconds in), then stop

#if defined(INPUTDRIVER_ENCODER_TYPE)
    // Poll the trackball double-click flag (~16x/sec, runs forever) and run the gesture:
    // wake / unlock / go Home / lock, depending on the current state (see handleHomeGesture).
    lv_timer_create(
        [](lv_timer_t *) {
            // Either the trackball double-click OR (when the screen is dark) a keyboard key
            // runs the gesture. The keyboard is the reliable wake for the stiff trackball.
            if (!tb_home_request && !tdeck_wake_request)
                return;
            tb_home_request = false;
            tdeck_wake_request = false;
            THIS->handleHomeGesture();
        },
        60, NULL);
#endif

    // Poll the Alt+C calibration request (keyboard-driven, so NOT gated on the trackball). When the
    // lock pad is up, Alt+C runs touch calibration then re-shows the pad — a reliable way to fix a
    // miscalibrated screen without needing a working touch to reach Settings.
    lv_timer_create(
        [](lv_timer_t *) {
            if (!tdeck_calib_request)
                return;
            tdeck_calib_request = false;
            // Only from the lock pad (the requested entry point, and the first screen after a wake).
            if (THIS->lockpad_screen && lv_screen_active() == THIS->lockpad_screen)
                THIS->startCalibrationFromLock();
        },
        60, NULL);

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    // The SD card often isn't mounted yet the instant the grid first builds — which leaves
    // user apps unscanned, uninstalls not applied, and the saved /apps/order.txt ignored
    // (falls back to default order). Once the card shows up, rebuild the grid ONCE so all of
    // that takes effect. Gives up after ~10s if no card ever appears.
    lv_timer_create(
        [](lv_timer_t *t) {
            static int tries = 0;
            if (sdCard) {
                tries = 0;
                lv_timer_delete(t);
                // Seed bundled apps HERE, now that the card is actually mounted — the earlier
                // createLauncher call runs before SD is up and silently installs nothing, which
                // left deleted (or never-installed) bundled apps missing forever.
                lua_seed_bundled();
                THIS->scanUserApps();
                THIS->rebuildAppGrid();
            } else if (++tries > 20) {
                tries = 0;
                lv_timer_delete(t);
            }
        },
        500, NULL);
#endif

    // Require the PIN at boot. A code is always in effect (default 1234 until the user sets
    // their own), so effectiveLockPin() is non-zero and the device boots locked. Reuses the
    // normal unlock path; bootHome above keeps the pad up through the config-sync window.
    if (effectiveLockPin() != 0) {
        lockState = LOCK_ENTRY;
        lockLen = 0;
        lockDigits[0] = 0;
        tdeck_hold_dark = false;
        tdeck_input_gated = false;
        showLockPad(false);
    }
}

// Discover user-added apps on the SD card: any /apps/<folder>/main.lua that isn't one
// of our built-in bundled apps. Runs once at boot. Fills userAppDirs[].
void TFTView_320x240::scanUserApps(void)
{
    userAppCount = 0;
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    if (!sdCard)
        return;
    FsFile dir = SDFs.open("/apps", O_RDONLY);
    if (!dir || !dir.isDirectory())
        return;
    FsFile entry;
    while (userAppCount < kMaxUserApps && (entry = dir.openNextFile())) {
        char nm[24];
        entry.getName(nm, sizeof(nm));
        bool isDir = entry.isDirectory();
        entry.close();
        if (!isDir || nm[0] == '.')
            continue;
        // our bundled apps already appear via kApps (with custom icons) — skip them here
        if (!strcmp(nm, "breakout") || !strcmp(nm, "metronome") || !strcmp(nm, "hello"))
            continue;
        char lp[48];
        snprintf(lp, sizeof(lp), "/apps/%s/main.lua", nm);
        if (SDFs.exists(lp)) {
            strncpy(userAppDirs[userAppCount], nm, sizeof(userAppDirs[0]) - 1);
            userAppDirs[userAppCount][sizeof(userAppDirs[0]) - 1] = 0;
            userAppCount++;
        }
    }
    dir.close();
#endif
}

void TFTView_320x240::runUserApp(int userIdx)
{
    if (userIdx < 0 || userIdx >= userAppCount)
        return;
    char path[48];
    snprintf(path, sizeof(path), "/apps/%s/main.lua", userAppDirs[userIdx]);
    lua_run_path(path);
}

// Reorder launchList to match the saved order in /apps/order.txt. Apps not listed
// (newly added) keep their default position at the end. Missing/garbled file => no-op
// (default order preserved). This is a best-effort convenience, never a hard failure.
void TFTView_320x240::applyAppOrder(void)
{
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    if (!sdCard || !SDFs.exists("/apps/order.txt"))
        return;
    FsFile f = SDFs.open("/apps/order.txt", O_RDONLY);
    if (!f)
        return;
    LaunchDesc ordered[40];
    bool placed[40] = {false};
    int oc = 0;
    char key[24];
    while (oc < launchCount && f.fgets(key, sizeof(key)) > 0) {
        for (char *c = key; *c; c++)
            if (*c == '\n' || *c == '\r') {
                *c = 0;
                break;
            }
        if (!key[0])
            continue;
        for (int i = 0; i < launchCount; i++)
            if (!placed[i] && !strcmp(launchList[i].name, key)) {
                ordered[oc++] = launchList[i];
                placed[i] = true;
                break;
            }
    }
    f.close();
    for (int i = 0; i < launchCount && oc < launchCount; i++) // append anything not listed
        if (!placed[i])
            ordered[oc++] = launchList[i];
    for (int i = 0; i < oc; i++)
        launchList[i] = ordered[i];
    launchCount = oc;
#endif
}

void TFTView_320x240::saveAppOrder(void)
{
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    if (!sdCard)
        return;
    SDFs.mkdir("/apps");
    FsFile f = SDFs.open("/apps/order.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return;
    for (int i = 0; i < launchCount; i++) {
        f.print(launchList[i].name);
        f.print("\n");
    }
    f.sync();
    f.close();
#endif
}

// Assemble launchList (built-in apps, SD-gated, then discovered user apps), apply the
// saved order, and build the pager + tiles + page dots. Long-press any tile to arrange.
// SAFETY: built-in apps are always included, so the grid is never empty even if the SD
// card is absent or misbehaving.
void TFTView_320x240::buildAppGrid(void)
{
    const int kPagerY = 26;
    const int kPagerH = 188;
    const int kPageW = 320;
    const int kPerPage = 6; // 3 columns × 2 rows
    const int kMax = (int)(sizeof(launchList) / sizeof(launchList[0]));

    launchCount = 0;
    for (size_t i = 0; i < sizeof(kApps) / sizeof(kApps[0]) && launchCount < kMax; i++) {
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
        if (kApps[i].sdPath && sdCard && !SDFs.exists(kApps[i].sdPath))
            continue; // uninstalled SD app -> hide its tile (fail-open if card unmounted)
#endif
        LaunchDesc &d = launchList[launchCount++];
        strncpy(d.name, kApps[i].name, sizeof(d.name) - 1);
        d.name[sizeof(d.name) - 1] = 0;
        d.color = kApps[i].color;
        d.builtinIdx = (int)i;
        d.userIdx = -1;
    }
    for (int u = 0; u < userAppCount && launchCount < kMax; u++) {
        LaunchDesc &d = launchList[launchCount++];
        strncpy(d.name, userAppDirs[u], sizeof(d.name) - 1);
        d.name[sizeof(d.name) - 1] = 0;
        if (d.name[0] >= 'a' && d.name[0] <= 'z')
            d.name[0] = (char)(d.name[0] - 32); // capitalize the folder name for display
        d.color = 0x5ac8fa;                     // generic tint for user apps
        d.builtinIdx = -1;
        d.userIdx = u;
    }
    applyAppOrder();

    size_t pageCount = (launchCount + kPerPage - 1) / kPerPage;
    if (pageCount == 0)
        pageCount = 1;

    lv_obj_t *pager = lv_obj_create(launcher_screen);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, kPageW, kPagerH);
    lv_obj_set_pos(pager, 0, kPagerY);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(pager, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(pager, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    launcher_pager = pager;

    // tap -> launch (built-in panel/action or a user Lua app); index into launchList
    auto tile_cb = [](lv_event_t *e) {
        if (THIS->suppressTileClick) { // a long-press just opened Arrange — swallow this click
            THIS->suppressTileClick = false;
            return;
        }
        int i = (int)(intptr_t)lv_event_get_user_data(e);
        if (i < 0 || i >= THIS->launchCount)
            return;
        TFTView_320x240::LaunchDesc &d = THIS->launchList[i];
        if (d.userIdx >= 0) {
            THIS->runUserApp(d.userIdx);
            return;
        }
        const LauncherApp *app = &kApps[d.builtinIdx];
        if (app->action) {
            app->action();
            return;
        }
        if (!app->panel)
            return;
        lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        // Fresh install, no LoRa region yet: Meshtastic already locked its sidebar
        // and queued the first-run setup panel (requestSetup), but the launcher was
        // covering it. Re-present setup instead of Home, or the user lands on a
        // dead-sidebar Home with no way to ever set the region (brother's install).
        if (app->panel == &objects.home_panel &&
            THIS->db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
            THIS->requestSetup();
            return;
        }
        THIS->ui_set_active(*app->button, *app->panel, *app->topPanel);
    };
    // long-press any tile -> open the Arrange screen (reorder apps). Set the suppress flag
    // AFTER openArrange() (which calls closeArrange -> clears it), so the release-click that
    // follows the long-press doesn't also launch the app underneath.
    auto tile_long_cb = [](lv_event_t *) {
        THIS->openArrange();
        THIS->suppressTileClick = true;
    };

    lv_obj_t *page = nullptr;
    for (int i = 0; i < launchCount; i++) {
        if (i % kPerPage == 0) {
            page = lv_obj_create(pager);
            lv_obj_remove_style_all(page);
            lv_obj_set_size(page, kPageW, kPagerH);
            lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_flex_align(page, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(page, 12, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(page, 12, LV_PART_MAIN);
        }
        lv_obj_t *tile = lv_btn_create(page);
        lv_obj_set_size(tile, 88, 84);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 16, LV_PART_MAIN);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, tile_long_cb, LV_EVENT_LONG_PRESSED, NULL);

        buildTileIcon(tile, launchList[i].name, launchList[i].color);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_label_set_text(lbl, launchList[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    if (pageCount > 1) {
        lv_obj_t *dots = lv_obj_create(launcher_screen);
        lv_obj_remove_style_all(dots);
        lv_obj_set_size(dots, kPageW, 14);
        lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dots, 6, LV_PART_MAIN);
        lv_obj_clear_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
        for (size_t p = 0; p < pageCount; p++) {
            lv_obj_t *d = lv_obj_create(dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, 7, 7);
            lv_obj_set_style_radius(d, 4, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(d, lv_color_hex(p == 0 ? 0xffffff : 0x48484a), LV_PART_MAIN);
        }
        launcher_dots = dots;
        lv_obj_add_event_cb(
            pager,
            [](lv_event_t *) {
                if (!THIS->launcher_pager || !THIS->launcher_dots)
                    return;
                int32_t sx = lv_obj_get_scroll_x(THIS->launcher_pager);
                int cur = (sx + 160) / 320;
                uint32_t n = lv_obj_get_child_count(THIS->launcher_dots);
                if (cur < 0)
                    cur = 0;
                if (cur > (int)n - 1)
                    cur = (int)n - 1;
                for (uint32_t k = 0; k < n; k++)
                    lv_obj_set_style_bg_color(lv_obj_get_child(THIS->launcher_dots, k),
                                              lv_color_hex((int)k == cur ? 0xffffff : 0x48484a), LV_PART_MAIN);
            },
            LV_EVENT_SCROLL, NULL);
    } else {
        launcher_dots = nullptr;
    }
}

// Rebuild the grid in place (after a reorder). Old tiles are destroyed first, so the
// index-based tile callbacks never dangle.
void TFTView_320x240::rebuildAppGrid(void)
{
    if (launcher_pager) {
        lv_obj_delete(launcher_pager);
        launcher_pager = nullptr;
    }
    if (launcher_dots) {
        lv_obj_delete(launcher_dots);
        launcher_dots = nullptr;
    }
    buildAppGrid();
}

void TFTView_320x240::closeArrange(void)
{
    suppressTileClick = false;
    if (arrange_overlay) {
        lv_obj_delete_async(arrange_overlay);
        arrange_overlay = nullptr;
        arrange_list = nullptr; // deleted along with the overlay
    }
}

// Re-label the arrange rows from the current order, in place. Doing this instead of
// rebuilding the overlay keeps the list exactly where the user scrolled it.
void TFTView_320x240::refreshArrangeRows(void)
{
    if (!arrange_list)
        return;
    uint32_t n = lv_obj_get_child_count(arrange_list);
    for (uint32_t i = 0; i < n && (int)i < launchCount; i++) {
        lv_obj_t *row = lv_obj_get_child(arrange_list, i);
        if (!row)
            continue;
        lv_obj_t *name = lv_obj_get_child(row, 0); // first child is the name label
        if (name)
            lv_label_set_text(name, launchList[i].name);
    }
}

// Swap two apps in the order, persist it, and re-apply live on the home screen.
void TFTView_320x240::moveAppInArrange(int idx, int delta)
{
    int j = idx + delta;
    if (idx < 0 || idx >= launchCount || j < 0 || j >= launchCount)
        return;
    LaunchDesc tmp = launchList[idx];
    launchList[idx] = launchList[j];
    launchList[j] = tmp;
    saveAppOrder();
    // Update the home grid behind the overlay, then re-label the visible rows in place.
    // (No overlay rebuild -> the scroll position is preserved.)
    lv_async_call([](void *) { THIS->rebuildAppGrid(); }, nullptr);
    refreshArrangeRows();
}

// "Arrange apps" overlay: every app in a row with ▲ / ▼ to move it. Reached by
// long-pressing any home-screen tile. Changes apply live and persist to the SD card.
void TFTView_320x240::openArrange(void)
{
    closeArrange();
    arrange_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(arrange_overlay);
    lv_obj_set_size(arrange_overlay, 320, 240);
    lv_obj_set_style_bg_color(arrange_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arrange_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(arrange_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(arrange_overlay);
    lv_label_set_text(title, "Arrange apps");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *doneBtn = lv_btn_create(arrange_overlay);
    lv_obj_set_size(doneBtn, 62, 28);
    lv_obj_align(doneBtn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_event_cb(doneBtn, [](lv_event_t *) { THIS->closeArrange(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(doneBtn);
    lv_label_set_text(dl, "Done");
    lv_obj_center(dl);

    lv_obj_t *list = lv_obj_create(arrange_overlay);
    arrange_list = list; // remembered so moves can re-label rows in place (keeps scroll)
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 314, 194);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < launchCount; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 302, 40);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, launchList[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_width(name, 180);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *up = lv_btn_create(row);
        lv_obj_set_size(up, 44, 30);
        lv_obj_align(up, LV_ALIGN_RIGHT_MID, -54, 0);
        lv_obj_add_event_cb(
            up,
            [](lv_event_t *e) { THIS->moveAppInArrange((int)(intptr_t)lv_event_get_user_data(e), -1); },
            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ul = lv_label_create(up);
        // Plain "<" text, not LV_SYMBOL_LEFT: the custom EEZ font has no FontAwesome glyphs, so
        // the symbol rendered as a blank blue square. "<" is in the font and reads as a left arrow.
        lv_label_set_text(ul, "<"); // move earlier in the order (left on the grid)
        lv_obj_set_style_text_font(ul, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(ul);

        lv_obj_t *dn = lv_btn_create(row);
        lv_obj_set_size(dn, 44, 30);
        lv_obj_align(dn, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_add_event_cb(
            dn,
            [](lv_event_t *e) { THIS->moveAppInArrange((int)(intptr_t)lv_event_get_user_data(e), +1); },
            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dnl = lv_label_create(dn);
        lv_label_set_text(dnl, ">"); // move later in the order (right on the grid)
        lv_obj_set_style_text_font(dnl, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_center(dnl);
    }
}

/**
 * @brief The mesh "kill switch". Single source of truth: updates the radio (via the
 *        firmware bridge), the status dot, and the settings switch so they never drift.
 */
void TFTView_320x240::setMeshEnabled(bool on)
{
    meshEnabled = on;
    tdeck_set_mesh_enabled(on); // park / restart the SX1262 in the firmware

    if (mesh_status_icon)
        lv_obj_set_style_bg_color(mesh_status_icon, lv_color_hex(on ? 0x30d158 : 0x8e8e93), LV_PART_MAIN);
    if (mesh_switch) {
        if (on)
            lv_obj_add_state(mesh_switch, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(mesh_switch, LV_STATE_CHECKED);
    }
}

/**
 * @brief The GPS on/off switch. On = GPS always searching (continuous)
 *        (Continuous = always looking); off = GPS powered down (saves battery).
 *        Applied live in the firmware via the control bridge — no reboot. Keeps the
 *        local config mirror in sync so the Maps "N sats / GPS off" readout stays correct.
 */
void TFTView_320x240::setGpsEnabled(bool on)
{
    gpsEnabled = on;
    if (on)
        tdeck_gps_set_interval(10); // <= the driver's always-on threshold: on means ALWAYS searching
    tdeck_gps_set_enabled(on);      // enable/disable the GPS driver in the firmware (deferred, no reboot)

    // Mirror the firmware config locally so the Maps screen and switch don't drift.
    db.config.position.gps_mode = on ? meshtastic_Config_PositionConfig_GpsMode_ENABLED
                                     : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
    if (on)
        db.config.position.gps_update_interval = 10;

    if (gps_switch) {
        if (on)
            lv_obj_add_state(gps_switch, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(gps_switch, LV_STATE_CHECKED);
    }
}

/**
 * @brief Refresh the launcher's top-bar battery %. Values come from updateMetrics()
 *        (own node telemetry). White normally, green when charging/plugged, red when low.
 */
void TFTView_320x240::updateLauncherBattery(void)
{
    if (!launcher_battery_label)
        return;
    if (launcherBatPct < 0) { // no telemetry yet
        lv_label_set_text(launcher_battery_label, "--");
        lv_obj_set_style_text_color(launcher_battery_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%%s", launcherBatPct, launcherBatPlugged ? "+" : "");
    lv_label_set_text(launcher_battery_label, buf);
    uint32_t col = 0xffffff;
    if (launcherBatPlugged)
        col = 0x30d158; // charging / external power
    else if (launcherBatPct <= 15)
        col = 0xff453a; // low
    lv_obj_set_style_text_color(launcher_battery_label, lv_color_hex(col), LV_PART_MAIN);
}

/**
 * @brief Build a minimal settings screen: title, a "Mesh radio" row with an on/off
 *        switch (the kill switch), and a Back button that returns to the grid.
 */
void TFTView_320x240::createSettingsScreen(void)
{
    settings_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scroll_dir(settings_screen, LV_DIR_VER); // scrollable now that there are more rows
    lv_obj_set_scrollbar_mode(settings_screen, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(settings_screen);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // (The old "Mesh radio" kill-switch row was removed at Jake's request — mesh just stays on.
    // setMeshEnabled/tdeck bridge remain, null-guarded on mesh_switch, if it ever comes back.)

    // "Lock PIN" row — opens the keypad in set mode to choose a new PIN
    lv_obj_t *pinLbl = lv_label_create(settings_screen);
    lv_label_set_text(pinLbl, "Lock PIN");
    lv_obj_set_style_text_color(pinLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(pinLbl, LV_ALIGN_TOP_LEFT, 16, 52);

    lv_obj_t *pinBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(pinBtn, 96, 30);
    lv_obj_align(pinBtn, LV_ALIGN_TOP_RIGHT, -16, 46);
    lv_obj_set_style_radius(pinBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        pinBtn, [](lv_event_t *e) { THIS->showLockPad(true); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pinBtnLbl = lv_label_create(pinBtn);
    lv_label_set_text(pinBtnLbl, "Change");
    lv_obj_center(pinBtnLbl);

    lv_obj_t *pinHint = lv_label_create(settings_screen);
    lv_obj_set_width(pinHint, 288);
    lv_label_set_long_mode(pinHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(pinHint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(pinHint, "Lock: double-click on Home. Default 1234");
    lv_obj_set_style_text_color(pinHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(pinHint, LV_ALIGN_TOP_LEFT, 16, 82);

    // "Brightness" row — live slider; persisted on release
    lv_obj_t *briLbl = lv_label_create(settings_screen);
    lv_label_set_text(briLbl, "Brightness");
    lv_obj_set_style_text_color(briLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(briLbl, LV_ALIGN_TOP_LEFT, 16, 114);

    lv_obj_t *briSlider = lv_slider_create(settings_screen);
    lv_slider_set_range(briSlider, 10, 255); // never let it slide fully dark
    lv_slider_set_value(briSlider, db.uiConfig.screen_brightness ? db.uiConfig.screen_brightness : 153, LV_ANIM_OFF);
    lv_obj_set_size(briSlider, 150, 14);
    lv_obj_align(briSlider, LV_ALIGN_TOP_RIGHT, -20, 116);
    lv_obj_add_event_cb(
        briSlider,
        [](lv_event_t *e) {
            lv_event_code_t code = lv_event_get_code(e);
            if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
                return;
            lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
            uint32_t v = lv_slider_get_value(s);
            THIS->setBrightness(v); // live preview while dragging
            if (code == LV_EVENT_RELEASED) {
                THIS->db.uiConfig.screen_brightness = v;
                THIS->controller->storeUIConfig(THIS->db.uiConfig); // survives reboot
            }
        },
        LV_EVENT_ALL, NULL);

    // "GPS" row — on = always searching (continuous); off = powered down
    lv_obj_t *gpsLbl = lv_label_create(settings_screen);
    lv_label_set_text(gpsLbl, "GPS");
    lv_obj_set_style_text_color(gpsLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(gpsLbl, LV_ALIGN_TOP_LEFT, 16, 152);

    gpsEnabled = tdeck_gps_get_enabled(); // reflect the firmware's current GPS state
    gps_switch = lv_switch_create(settings_screen);
    lv_obj_set_size(gps_switch, 56, 28);
    lv_obj_align(gps_switch, LV_ALIGN_TOP_RIGHT, -16, 146);
    lv_obj_set_style_bg_color(gps_switch, lv_color_hex(0x30d158), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (gpsEnabled)
        lv_obj_add_state(gps_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        gps_switch,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            THIS->setGpsEnabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, NULL);

    // "Share location" row — Meshtastic's own per-channel position precision (primary channel).
    // Exact = 32 bits, Rough = 13 (~1.5 km area, the Meshtastic default), Off = never send.
    // On public-key channels the firmware itself clamps anything finer than ~350 m — rule kept.
    lv_obj_t *locLbl = lv_label_create(settings_screen);
    lv_label_set_text(locLbl, "Share location");
    lv_obj_set_style_text_color(locLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(locLbl, LV_ALIGN_TOP_LEFT, 16, 186);

    lv_obj_t *locBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(locBtn, 112, 32);
    lv_obj_align(locBtn, LV_ALIGN_TOP_RIGHT, -16, 180);
    lv_obj_set_style_radius(locBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        locBtn, [](lv_event_t *) { THIS->cycleLocPrecision(); }, LV_EVENT_CLICKED, NULL);
    loc_precision_label = lv_label_create(locBtn);
    lv_obj_center(loc_precision_label);
    updateLocPrecisionLabel();

    lv_obj_t *gpsHint = lv_label_create(settings_screen);
    lv_obj_set_width(gpsHint, 288); // bounded + wrapped so it never runs off-screen
    lv_label_set_long_mode(gpsHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(gpsHint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(gpsHint, "GPS on = always searching, even while asleep. Share location = how exactly others on the mesh see you.");
    lv_obj_set_style_text_color(gpsHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(gpsHint, LV_ALIGN_TOP_LEFT, 16, 222);

    // ---- WiFi section ----
    lv_obj_t *wifiHdr = lv_label_create(settings_screen);
    lv_label_set_text(wifiHdr, "WiFi");
    lv_obj_set_style_text_color(wifiHdr, lv_color_hex(0x0a84ff), LV_PART_MAIN);
    lv_obj_align(wifiHdr, LV_ALIGN_TOP_LEFT, 16, 266);

    // Network name -> keyboard (button shows the saved name)
    lv_obj_t *netLbl = lv_label_create(settings_screen);
    lv_label_set_text(netLbl, "Network");
    lv_obj_set_style_text_color(netLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(netLbl, LV_ALIGN_TOP_LEFT, 16, 298);

    lv_obj_t *netBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(netBtn, 150, 30);
    lv_obj_align(netBtn, LV_ALIGN_TOP_RIGHT, -16, 292);
    lv_obj_set_style_radius(netBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        netBtn, [](lv_event_t *) { THIS->wifiScanOpen(); }, LV_EVENT_CLICKED, NULL);
    wifi_net_btn_lbl = lv_label_create(netBtn);
    lv_obj_set_width(wifi_net_btn_lbl, 134);
    lv_label_set_long_mode(wifi_net_btn_lbl, LV_LABEL_LONG_DOT);
    lv_obj_center(wifi_net_btn_lbl);
    updateWifiNetLabel();

    // Password -> keyboard (masked)
    lv_obj_t *pwLbl = lv_label_create(settings_screen);
    lv_label_set_text(pwLbl, "Password");
    lv_obj_set_style_text_color(pwLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(pwLbl, LV_ALIGN_TOP_LEFT, 16, 330);

    lv_obj_t *pwBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(pwBtn, 96, 30);
    lv_obj_align(pwBtn, LV_ALIGN_TOP_RIGHT, -16, 324);
    lv_obj_set_style_radius(pwBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        pwBtn, [](lv_event_t *) { THIS->wifiEntryPrompt(true); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pwBtnLbl = lv_label_create(pwBtn);
    lv_label_set_text(pwBtnLbl, "Set");
    lv_obj_center(pwBtnLbl);

    // WiFi on/off — applying writes the network config (device reboots to connect)
    lv_obj_t *wifiLbl = lv_label_create(settings_screen);
    lv_label_set_text(wifiLbl, "Turn WiFi on");
    lv_obj_set_style_text_color(wifiLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(wifiLbl, LV_ALIGN_TOP_LEFT, 16, 366);

    wifi_switch = lv_switch_create(settings_screen);
    lv_obj_set_size(wifi_switch, 56, 28);
    lv_obj_align(wifi_switch, LV_ALIGN_TOP_RIGHT, -16, 360);
    lv_obj_set_style_bg_color(wifi_switch, lv_color_hex(0x30d158), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (db.config.network.wifi_enabled)
        lv_obj_add_state(wifi_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(
        wifi_switch,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            THIS->wifiApplyEnabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, NULL);

    wifi_status_label = lv_label_create(settings_screen);
    lv_obj_set_width(wifi_status_label, 288);
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_LEFT, 16, 394);
    updateWifiStatus();

    lv_obj_t *wifiHint = lv_label_create(settings_screen);
    lv_obj_set_width(wifiHint, 288);
    lv_label_set_long_mode(wifiHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(wifiHint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(wifiHint, "Set the name + password, then turn WiFi on.\nIt restarts the device and pauses Bluetooth.");
    lv_obj_set_style_text_color(wifiHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(wifiHint, LV_ALIGN_TOP_LEFT, 16, 418);

    // refresh the status line every couple seconds while this screen is up
    wifi_status_timer = lv_timer_create([](lv_timer_t *) { THIS->updateWifiStatus(); }, 2000, NULL);

    // File Share (FTP) — opens the on-demand share screen (turns WiFi on just for transfers)
    lv_obj_t *shareLbl = lv_label_create(settings_screen);
    lv_label_set_text(shareLbl, "Share files (Wi-Fi)");
    lv_obj_set_style_text_color(shareLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(shareLbl, LV_ALIGN_TOP_LEFT, 16, 460);

    lv_obj_t *shareBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(shareBtn, 96, 30);
    lv_obj_align(shareBtn, LV_ALIGN_TOP_RIGHT, -16, 454);
    lv_obj_set_style_radius(shareBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        shareBtn, [](lv_event_t *) { THIS->openFileShare(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *shareBtnLbl = lv_label_create(shareBtn);
    lv_label_set_text(shareBtnLbl, "Open");
    lv_obj_center(shareBtnLbl);

    // "Screen timeout" row — how long until the screen dims to dark (then PIN to wake).
    // Cycle button; persists in uiConfig like brightness.
    lv_obj_t *toLbl = lv_label_create(settings_screen);
    lv_label_set_text(toLbl, "Screen timeout");
    lv_obj_set_style_text_color(toLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(toLbl, LV_ALIGN_TOP_LEFT, 16, 500);

    lv_obj_t *toBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(toBtn, 112, 32);
    lv_obj_align(toBtn, LV_ALIGN_TOP_RIGHT, -16, 494);
    lv_obj_set_style_radius(toBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        toBtn, [](lv_event_t *) { THIS->cycleScreenTimeout(); }, LV_EVENT_CLICKED, NULL);
    timeout_btn_label = lv_label_create(toBtn);
    lv_obj_center(timeout_btn_label);
    updateTimeoutBtnLabel();

    // "Sound" row — off = mute the beeps (games, metronome, timer alarm, Lua apps).
    lv_obj_t *sndLbl = lv_label_create(settings_screen);
    lv_label_set_text(sndLbl, "Sound");
    lv_obj_set_style_text_color(sndLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(sndLbl, LV_ALIGN_TOP_LEFT, 16, 542);

    mute_switch = lv_switch_create(settings_screen);
    lv_obj_align(mute_switch, LV_ALIGN_TOP_RIGHT, -16, 536);
    if (tdeck_sound_get_enabled())
        lv_obj_add_state(mute_switch, LV_STATE_CHECKED); // switch ON = sound ON
    lv_obj_add_event_cb(
        mute_switch,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            // drives Meshtastic's own buzzer_mode: covers game/timer beeps AND message alerts
            tdeck_sound_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sndHint = lv_label_create(settings_screen);
    lv_obj_set_width(sndHint, 288);
    lv_label_set_long_mode(sndHint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(sndHint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(sndHint, "Off = silence everything, including message alerts.");
    lv_obj_set_style_text_color(sndHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(sndHint, LV_ALIGN_TOP_LEFT, 16, 570);

    // "Time zone" row — makes the clock on the home screen read local time. The device gets
    // UTC from the GPS satellites; without a zone the firmware falls back to GMT, which is
    // why the clock would otherwise be hours out. Order must match kTdeckZones[].
    lv_obj_t *tzLbl = lv_label_create(settings_screen);
    lv_label_set_text(tzLbl, "Time zone");
    lv_obj_set_style_text_color(tzLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(tzLbl, LV_ALIGN_TOP_LEFT, 16, 602);

    lv_obj_t *tzDd = lv_dropdown_create(settings_screen);
    lv_dropdown_set_options(tzDd, "Pacific\nMountain\nArizona\nCentral\nEastern\nAlaska\nHawaii\n"
                                  "UTC\nUK\nCentral Europe");
    lv_obj_set_width(tzDd, 150);
    lv_obj_align(tzDd, LV_ALIGN_TOP_RIGHT, -16, 596);
    {
        int cur = tdeck_tz_get();
        if (cur >= 0)
            lv_dropdown_set_selected(tzDd, (uint32_t)cur);
    }
    lv_obj_add_event_cb(
        tzDd,
        [](lv_event_t *e) { tdeck_tz_set((int)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e))); },
        LV_EVENT_VALUE_CHANGED, NULL);

    // Back to the grid
    lv_obj_t *backBtn = lv_btn_create(settings_screen);
    lv_obj_set_size(backBtn, 90, 34);
    lv_obj_align(backBtn, LV_ALIGN_TOP_MID, 0, 648);
    lv_obj_set_style_radius(backBtn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(
        backBtn,
        [](lv_event_t *e) {
            if (THIS->launcher_screen)
                lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *backLbl = lv_label_create(backBtn);
    lv_label_set_text(backLbl, "Back");
    lv_obj_center(backLbl);

    // Version, last line on the screen — the only place the launcher's own version is visible.
    lv_obj_t *verLbl = lv_label_create(settings_screen);
    lv_obj_set_style_text_font(verLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(verLbl, "Version " TUI_VERSION);
    lv_obj_set_style_text_color(verLbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(verLbl, LV_ALIGN_TOP_MID, 0, 692);
}

/**
 * @brief Open the settings screen (lazily built on first use).
 */
void TFTView_320x240::openSettings(void)
{
    if (!settings_screen)
        createSettingsScreen();
    lv_screen_load_anim(settings_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Public trampoline so file-scope launcher tiles can open Settings.
void TFTView_320x240::openSettingsAction(void)
{
    instance()->openSettings();
}

// -------------------------------------------------------------------------------------------
// File Share (FTP over WiFi), on-demand. Opening this screen brings WiFi up (no reboot) using
// the saved network, starts an FTP server on the SD card, and shows the address + login. Closing
// it (Back or double-click-Home) stops the server and powers WiFi back down, so WiFi's RAM cost
// is only paid while you're actually transferring. Login is a fixed tdeck / 1234 for now.
// -------------------------------------------------------------------------------------------
void TFTView_320x240::openFileShare(void)
{
    if (!fileshare_screen) {
        fileshare_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(fileshare_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(fileshare_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(fileshare_screen);
        lv_label_set_text(title, "Share files over Wi-Fi");
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        fileshare_status_label = lv_label_create(fileshare_screen);
        lv_obj_set_width(fileshare_status_label, 300);
        lv_obj_set_style_text_color(fileshare_status_label, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_align(fileshare_status_label, LV_ALIGN_TOP_MID, 0, 46);

        fileshare_info_label = lv_label_create(fileshare_screen);
        lv_obj_set_width(fileshare_info_label, 300);
        lv_label_set_long_mode(fileshare_info_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(fileshare_info_label, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(fileshare_info_label, LV_ALIGN_TOP_LEFT, 12, 84);
        lv_label_set_text(fileshare_info_label, "");

        lv_obj_t *backBtn = lv_btn_create(fileshare_screen);
        lv_obj_set_size(backBtn, 140, 40);
        lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_add_event_cb(
            backBtn,
            [](lv_event_t *) {
                THIS->closeFileShare();
                if (THIS->settings_screen)
                    lv_screen_load_anim(THIS->settings_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                else if (THIS->launcher_screen)
                    lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            },
            LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(backBtn);
        lv_label_set_text(bl, "Done");
        lv_obj_center(bl);

        // Safety net: leaving via double-click-Home also tears WiFi + FTP down.
        lv_obj_add_event_cb(
            fileshare_screen, [](lv_event_t *) { THIS->closeFileShare(); }, LV_EVENT_SCREEN_UNLOADED, NULL);
    }

    lv_label_set_text(fileshare_info_label, "");
    lv_screen_load_anim(fileshare_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

    // If WiFi is already up (e.g. the user turned it on in Settings), use it as-is and DON'T tear
    // it down on exit. Otherwise bring it up on-demand from the saved network and drop it on exit.
    bool already = tdeck_wifi_connected();
    fileshare_own_wifi = !already;
    if (!already && !tdeck_wifi_connect_now(db.config.network.wifi_ssid, db.config.network.wifi_psk)) {
        lv_label_set_text(fileshare_status_label, "No Wi-Fi network saved");
        lv_label_set_text(fileshare_info_label,
                          "Set one first: Settings -> Wi-Fi -> Network + Password, then come back here.");
        return;
    }

    fileshare_ftp_up = false;
    fileshare_deadline = lv_tick_get() + 25000; // ~25s to associate before we give up
    lv_label_set_text(fileshare_status_label,
                      already ? "Wi-Fi on, starting share..." : "Turning Wi-Fi on, connecting...");

    if (!fileshare_timer) {
        // One fast timer: pump the FTP server every tick AND advance the connect state machine.
        fileshare_timer = lv_timer_create(
            [](lv_timer_t *) {
                // Keep the screen awake for the WHOLE sharing session — otherwise it idle-dims,
                // locks, and waking it jumps to Home and tears the FTP server down mid-transfer.
                lv_display_trigger_activity(NULL);
                if (THIS->fileshare_ftp_up) {
                    tdeck_ftp_service(); // keep transfers moving
                    return;
                }
                if (tdeck_wifi_connected()) {
                    tdeck_ftp_start("tdeck", "1234");
                    THIS->fileshare_ftp_up = true;
                    char ip[24] = {0};
                    tdeck_wifi_ip(ip, sizeof(ip));
                    lv_label_set_text(THIS->fileshare_status_label, "Ready! Keep this screen open.");
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "On your computer, open:\n  ftp://%s\n\nUser:  tdeck\nPass:  1234\n\n"
                             "Drag files to/from the SD card.",
                             ip[0] ? ip : "?");
                    lv_label_set_text(THIS->fileshare_info_label, msg);
                } else if (lv_tick_get() > THIS->fileshare_deadline) {
                    lv_label_set_text(THIS->fileshare_status_label, "Couldn't connect to Wi-Fi");
                    lv_label_set_text(THIS->fileshare_info_label,
                                      "Check the network + password in Settings -> Wi-Fi, then try again.");
                    if (THIS->fileshare_own_wifi)
                        tdeck_wifi_disconnect_now();
                    if (THIS->fileshare_timer) {
                        lv_timer_delete(THIS->fileshare_timer);
                        THIS->fileshare_timer = nullptr;
                    }
                }
            },
            40, NULL);
    }
}

void TFTView_320x240::closeFileShare(void)
{
    if (fileshare_timer) {
        lv_timer_delete(fileshare_timer);
        fileshare_timer = nullptr;
    }
    tdeck_ftp_stop();
    if (fileshare_own_wifi) // only drop WiFi if WE brought it up (leave the Settings toggle alone)
        tdeck_wifi_disconnect_now();
    fileshare_ftp_up = false;
}

// "Share location" — Meshtastic's own per-channel position precision, applied to the
// primary channel with the exact same admin path the channel-mute long-press uses (no
// reboot). Exact = 32 bits, Rough = 13 (~1.5 km area, the Meshtastic default), Off = 0
// (never send position). NOTE the firmware independently clamps public-key channels to
// ~350 m (MAX_POSITION_PRECISION_PUBLIC_KEY) — "Exact" is only truly exact on a private
// channel, which is Meshtastic's privacy rule, not ours to bypass.
void TFTView_320x240::updateLocPrecisionLabel(void)
{
    if (!loc_precision_label)
        return;
    uint32_t p = db.channel[0].settings.module_settings.position_precision;
    lv_label_set_text(loc_precision_label, p == 0 ? "Off" : (p >= 24 ? "Exact" : "Rough"));
}

void TFTView_320x240::cycleLocPrecision(void)
{
    uint32_t p = db.channel[0].settings.module_settings.position_precision;
    uint32_t next = (p >= 24) ? 13 : (p == 0 ? 32 : 0); // Exact -> Rough -> Off -> Exact
    db.channel[0].settings.module_settings.position_precision = next;
    db.channel[0].settings.has_module_settings = true;
    db.channel[0].has_settings = true;
    updateChannelConfig(db.channel[0]);
    controller->sendConfig(db.channel[0], ownNode);
    updateLocPrecisionLabel();
}

// Settings "Screen timeout" cycle. 0 = never dim.
namespace
{
constexpr uint32_t kTimeoutSecs[] = {15, 30, 60, 120, 300, 0};
constexpr const char *kTimeoutNames[] = {"15 sec", "30 sec", "1 min", "2 min", "5 min", "Never"};
constexpr int kTimeoutCount = sizeof(kTimeoutSecs) / sizeof(kTimeoutSecs[0]);

int timeoutIndexForSecs(uint32_t secs)
{
    for (int i = 0; i < kTimeoutCount; i++) {
        if (kTimeoutSecs[i] == secs)
            return i;
    }
    return 1; // unknown stored value -> treat as the 30s default
}
} // namespace

void TFTView_320x240::updateTimeoutBtnLabel(void)
{
    if (!timeout_btn_label)
        return;
    lv_label_set_text(timeout_btn_label, kTimeoutNames[timeoutIndexForSecs(db.uiConfig.screen_timeout)]);
}

void TFTView_320x240::cycleScreenTimeout(void)
{
    int idx = timeoutIndexForSecs(db.uiConfig.screen_timeout);
    idx = (idx + 1) % kTimeoutCount;
    db.uiConfig.screen_timeout = kTimeoutSecs[idx];
    setTimeout(kTimeoutSecs[idx]);          // live apply (display driver + MUI settings label)
    controller->storeUIConfig(db.uiConfig); // survives reboot
    updateTimeoutBtnLabel();
}

// -----------------------------------------------------------------------------
// Settings: WiFi. The network name + password are typed on-device (so the
// password never leaves the T-Deck). Turning WiFi on writes Meshtastic's network
// config and applies it — the firmware reboots to connect, and because WiFi and
// Bluetooth share the radio, enabling WiFi disables BT pairing (mesh is separate
// and unaffected). Live status comes from the TDeckWifi firmware bridge.
// -----------------------------------------------------------------------------
void TFTView_320x240::updateWifiNetLabel(void)
{
    if (!wifi_net_btn_lbl)
        return;
    const char *ssid = db.config.network.wifi_ssid;
    lv_label_set_text(wifi_net_btn_lbl, (ssid && ssid[0]) ? ssid : "Set name");
}

void TFTView_320x240::updateWifiStatus(void)
{
    if (!wifi_status_label)
        return;
    if (settings_screen && lv_screen_active() != settings_screen) // only while the screen is up
        return;
    char buf[80];
    int st = tdeck_wifi_state();
    if (!db.config.network.wifi_enabled || st == 0) {
        snprintf(buf, sizeof(buf), "Status: off");
    } else if (st == 2) {
        char ip[24] = {};
        tdeck_wifi_ip(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "Connected   %s", ip);
    } else {
        snprintf(buf, sizeof(buf), "Connecting to %s ...", db.config.network.wifi_ssid);
    }
    lv_label_set_text(wifi_status_label, buf);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(st == 2 ? 0x30d158 : 0x8e8e93), LV_PART_MAIN);
}

void TFTView_320x240::wifiApplyEnabled(bool enable)
{
    meshtastic_Config_NetworkConfig &net = db.config.network;
    if (enable && !net.wifi_ssid[0]) { // nothing to connect to — bounce the switch back
        messageAlert(_("Set a network name first"), true);
        if (wifi_switch)
            lv_obj_remove_state(wifi_switch, LV_STATE_CHECKED);
        return;
    }
    net.wifi_enabled = enable;
    controller->sendConfig(meshtastic_Config_NetworkConfig{net}); // persist + apply (device reboots)
    updateWifiStatus();
}

void TFTView_320x240::closeWifiEntry(void)
{
    if (wifi_entry_guard) { // stop the focus guard before the textarea it points at is gone
        lv_timer_delete(wifi_entry_guard);
        wifi_entry_guard = nullptr;
    }
    if (wifi_ovl) {
        lv_obj_delete_async(wifi_ovl);
        wifi_ovl = nullptr;
        wifi_ta = nullptr;
    }
}

// Save the typed value and close. Shared by the Enter key and the OK button.
void TFTView_320x240::wifiEntryCommit(void)
{
    if (!wifi_ta)
        return;
    const char *txt = lv_textarea_get_text(wifi_ta);
    if (wifi_entry_psk) {
        strncpy(db.config.network.wifi_psk, txt, sizeof(db.config.network.wifi_psk) - 1);
        db.config.network.wifi_psk[sizeof(db.config.network.wifi_psk) - 1] = 0;
    } else {
        strncpy(db.config.network.wifi_ssid, txt, sizeof(db.config.network.wifi_ssid) - 1);
        db.config.network.wifi_ssid[sizeof(db.config.network.wifi_ssid) - 1] = 0;
        updateWifiNetLabel();
    }
    closeWifiEntry();
}

// Type the network name (psk=false) or password (psk=true) on the T-Deck's PHYSICAL
// keyboard — no on-screen keyboard. The textarea joins LVGL's default input group
// (the same way the mesh chat input is typed into), with a guard that re-grabs focus
// if the trackball nudges it. Enter or the OK button confirms.
void TFTView_320x240::wifiEntryPrompt(bool psk)
{
    wifi_entry_psk = psk;

    wifi_ovl = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wifi_ovl);
    lv_obj_set_size(wifi_ovl, 320, 240);
    lv_obj_set_style_bg_color(wifi_ovl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_ovl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(wifi_ovl);
    lv_label_set_text(title, psk ? "WiFi password" : "WiFi network name");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 18);

    lv_obj_t *hint = lv_label_create(wifi_ovl);
    lv_label_set_text(hint, "Type on the keyboard, then Enter or OK");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 14, 44);

    wifi_ta = lv_textarea_create(wifi_ovl);
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, psk);
    lv_textarea_set_max_length(wifi_ta, psk ? sizeof(db.config.network.wifi_psk) - 1
                                            : sizeof(db.config.network.wifi_ssid) - 1);
    lv_textarea_set_text(wifi_ta, psk ? db.config.network.wifi_psk : db.config.network.wifi_ssid);
    lv_obj_set_width(wifi_ta, 292);
    lv_obj_align(wifi_ta, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_add_event_cb(wifi_ta, [](lv_event_t *) { THIS->wifiEntryCommit(); }, LV_EVENT_READY, NULL);

    if (lv_group_get_default()) {
        lv_group_add_obj(lv_group_get_default(), wifi_ta);
        lv_group_focus_obj(wifi_ta);
    }
    wifi_entry_guard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            if (g && THIS->wifi_ta && lv_group_get_focused(g) != THIS->wifi_ta)
                lv_group_focus_obj(THIS->wifi_ta);
        },
        400, NULL);

    lv_obj_t *okBtn = lv_btn_create(wifi_ovl);
    lv_obj_set_size(okBtn, 96, 38);
    lv_obj_align(okBtn, LV_ALIGN_BOTTOM_RIGHT, -16, -26);
    lv_obj_set_style_radius(okBtn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(okBtn, lv_color_hex(0x0a84ff), LV_PART_MAIN);
    lv_obj_add_event_cb(okBtn, [](lv_event_t *) { THIS->wifiEntryCommit(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(okBtn);
    lv_label_set_text(okl, "OK");
    lv_obj_center(okl);

    lv_obj_t *cxBtn = lv_btn_create(wifi_ovl);
    lv_obj_set_size(cxBtn, 96, 38);
    lv_obj_align(cxBtn, LV_ALIGN_BOTTOM_LEFT, 16, -26);
    lv_obj_set_style_radius(cxBtn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cxBtn, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_add_event_cb(cxBtn, [](lv_event_t *) { THIS->closeWifiEntry(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cxl = lv_label_create(cxBtn);
    lv_label_set_text(cxl, "Cancel");
    lv_obj_center(cxl);
}

void TFTView_320x240::wifiScanClose(void)
{
    if (wifi_scan_timer) {
        lv_timer_delete(wifi_scan_timer);
        wifi_scan_timer = nullptr;
    }
    tdeck_wifi_scan_free();
    if (wifi_scan_ovl) {
        lv_obj_delete_async(wifi_scan_ovl);
        wifi_scan_ovl = nullptr;
        wifi_scan_list = nullptr;
        wifi_scan_status = nullptr;
    }
}

// Tap "Network" -> scan for nearby WiFi and show them to pick from. Falls back to
// typing (also the way to join a hidden network) via the "Type it in" button.
void TFTView_320x240::wifiScanOpen(void)
{
    wifi_scan_ovl = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wifi_scan_ovl);
    lv_obj_set_size(wifi_scan_ovl, 320, 240);
    lv_obj_set_style_bg_color(wifi_scan_ovl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_scan_ovl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_scan_ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(wifi_scan_ovl);
    lv_label_set_text(title, "Choose WiFi network");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    wifi_scan_status = lv_label_create(wifi_scan_ovl);
    lv_label_set_text(wifi_scan_status, "Scanning...");
    lv_obj_set_style_text_color(wifi_scan_status, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(wifi_scan_status, LV_ALIGN_TOP_LEFT, 12, 34);

    wifi_scan_list = lv_list_create(wifi_scan_ovl);
    lv_obj_set_size(wifi_scan_list, 300, 148);
    lv_obj_align(wifi_scan_list, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(wifi_scan_list, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_scan_list, 0, LV_PART_MAIN);

    // Type it in (fallback + the only way to join a hidden network)
    lv_obj_t *manualBtn = lv_btn_create(wifi_scan_ovl);
    lv_obj_set_size(manualBtn, 130, 30);
    lv_obj_align(manualBtn, LV_ALIGN_BOTTOM_LEFT, 8, -6);
    lv_obj_set_style_radius(manualBtn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        manualBtn,
        [](lv_event_t *) {
            THIS->wifiScanClose();
            lv_async_call([](void *) { THIS->wifiEntryPrompt(false); }, nullptr);
        },
        LV_EVENT_CLICKED, NULL);
    lv_obj_t *mlbl = lv_label_create(manualBtn);
    lv_label_set_text(mlbl, "Type it in");
    lv_obj_center(mlbl);

    lv_obj_t *cancelBtn = lv_btn_create(wifi_scan_ovl);
    lv_obj_set_size(cancelBtn, 100, 30);
    lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_radius(cancelBtn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_add_event_cb(
        cancelBtn, [](lv_event_t *) { THIS->wifiScanClose(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancelBtn);
    lv_label_set_text(clbl, "Cancel");
    lv_obj_center(clbl);

    tdeck_wifi_scan_start();
    wifi_scan_timer = lv_timer_create([](lv_timer_t *) { THIS->wifiScanPoll(); }, 700, NULL);
}

void TFTView_320x240::wifiScanPoll(void)
{
    int r = tdeck_wifi_scan_done();
    if (r == -1) // still scanning
        return;
    if (wifi_scan_timer) { // finished or failed — stop polling
        lv_timer_delete(wifi_scan_timer);
        wifi_scan_timer = nullptr;
    }
    if (r <= 0) {
        if (wifi_scan_status)
            lv_label_set_text(wifi_scan_status,
                              r == 0 ? "None found. Tap \"Type it in\"." : "Couldn't scan. Tap \"Type it in\".");
        return;
    }

    int shown = 0;
    for (int i = 0; i < r && shown < 16; i++) {
        char ssid[33] = {};
        int rssi = 0, secure = 0;
        tdeck_wifi_scan_get(i, ssid, sizeof(ssid), &rssi, &secure);
        if (!ssid[0])
            continue;
        bool dup = false; // mesh APs advertise the same SSID many times
        for (int k = 0; k < shown; k++)
            if (strcmp(wifi_scan_ssids[k], ssid) == 0) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        snprintf(wifi_scan_ssids[shown], sizeof(wifi_scan_ssids[0]), "%s", ssid);

        lv_obj_t *btn = lv_list_add_button(wifi_scan_list, LV_SYMBOL_WIFI, wifi_scan_ssids[shown]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                strncpy(THIS->db.config.network.wifi_ssid, THIS->wifi_scan_ssids[idx],
                        sizeof(THIS->db.config.network.wifi_ssid) - 1);
                THIS->db.config.network.wifi_ssid[sizeof(THIS->db.config.network.wifi_ssid) - 1] = 0;
                THIS->updateWifiNetLabel();
                THIS->wifiScanClose();
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)shown);
        shown++;
    }
    if (wifi_scan_status)
        lv_label_set_text(wifi_scan_status, shown ? "Tap your network:" : "None found. Tap \"Type it in\".");
}

// -----------------------------------------------------------------------------
// Files app — browse SD card + T-Deck internal memory, with copy & paste.
//
// SD access uses the same SdFat instance (SDFs) the map-tile loader reads from,
// on the UI task, so no new cross-thread hardware access is introduced. Internal
// flash is the same LittleFS the firmware stores its data on. Listing is HARD
// CAPPED at kFilesMaxEntries per directory: the card holds ~600k map tiles and
// enumerating a huge directory to the end would stall the UI. openNextFile()
// walks entries one at a time, so stopping early is cheap and safe.
// -----------------------------------------------------------------------------
void TFTView_320x240::openFilesAction(void)
{
    instance()->openFiles();
}

// -----------------------------------------------------------------------------
// Flashlight app — full-white screen at max backlight. Tap anywhere (or the usual
// double-click Home) to exit; brightness is restored on the way out. A slow timer
// keeps LVGL's inactivity clock reset so the screen never idle-dims while it's up.
// -----------------------------------------------------------------------------
void TFTView_320x240::openFlashlightAction(void)
{
    instance()->openFlashlight();
}

// -----------------------------------------------------------------------------
// Maps app — our OWN standalone map screen, fully separate from Meshtastic's map.
// Same SD-card x/y/z tiles (via a second MapPanel instance on our own screen), but
// its own top bar (Back / zoom / Me / Pins) and the pin features live only here.
// Swipe scrolls, long-press drops a pin, double-click Home exits like any app.
//
// Shared-state cautions (both maps coexist in one firmware):
//  - The SD tile service ctor registers a static LVGL fs driver -> sharedTileService()
//    guarantees exactly one instance, used by both this map and the mesh map.
//  - MapTileSettings (zoom/prefix/style) is global, and MapPanel::redraw() keeps
//    function-local statics. Each map therefore re-syncs with setZoom() + a full
//    tile rebuild when it comes back on screen (see meshMapStale / openMaps()).
// -----------------------------------------------------------------------------
void TFTView_320x240::openMapsAction(void)
{
    instance()->openMaps();
}

ITileService *TFTView_320x240::sharedTileService(void)
{
#if LV_USE_FS_ARDUINO_SD
    return nullptr;
#elif defined(HAS_SD_MMC)
    static ITileService *svc = new SDCardService();
    return svc;
#elif defined(HAS_SDCARD)
    static ITileService *svc = new SdFatService();
    return svc;
#elif defined(ARCH_PORTDUINO)
    static ITileService *svc = new SDCardService();
    return svc;
#else
    static ITileService *svc = new URLService();
    return svc;
#endif
}

// Same style/prefix detection the mesh map does in loadMap(), minus its dropdown UI,
// so the Maps app finds the tiles even if the mesh map was never opened this boot.
// Restore the map style the user last chose. Two traps this has to avoid:
//
//  1. The saved style arrives with the radio's UI config, which syncs several seconds AFTER
//     boot. Opening Maps before that lands used to read an EMPTY style, fail the lookup, fall
//     back to whichever folder sorts first alphabetically (e.g. "osm" beating "usgs"), and
//     then latch mapsStyleInited so it never corrected itself. Now: if the config hasn't
//     arrived yet we apply a provisional style but DON'T latch, so the next open gets it right.
//  2. A style is three things - the folder, its image format (.format) and its tile server
//     (.url). This used to restore only the folder, leaving format/server at defaults, so the
//     "right" map could still draw wrong. Routing through mapsApplyStyle restores all three.
void TFTView_320x240::mapsInitTileStyle(void)
{
    if (mapsStyleInited || !sdCard)
        return;
    std::set<std::string> mapStyles = sdCard->loadMapStyles(MapTileSettings::getPrefix());
    if (mapStyles.find("/map") != mapStyles.end()) {
        MapTileSettings::setPrefix("/map");
        MapTileSettings::setTileStyle("");
        mapsStyleInited = true;
        return;
    }
    if (mapStyles.empty())
        return; // no card / no styles yet - try again next time Maps opens

    // The prefix has to be right BEFORE mapsApplyStyle reads .format and .url from the card.
    MapTileSettings::setPrefix("/maps");

    const char *saved = db.uiConfig.map_data.style;
    bool haveSaved = (saved[0] != 0);
    bool savedOnCard = haveSaved && (mapStyles.find(saved) != mapStyles.end());

    // persist=false throughout: restoring a preference must never overwrite it. Otherwise a
    // fallback pick would quietly become the new saved choice.
    mapsApplyStyle(savedOnCard ? saved : mapStyles.begin()->c_str(), false);

    // Only consider this settled once we've actually honoured a saved choice. If the config
    // hasn't synced yet (no saved style at all), leave the door open to correct ourselves.
    mapsStyleInited = savedOnCard || haveSaved;
}

void TFTView_320x240::openMaps(void)
{
    if (!maps_screen) {
        maps_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(maps_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(maps_screen, LV_OBJ_FLAG_SCROLLABLE);

        // tile canvas below the top bar; children outside its area are clipped
        maps_map_container = lv_obj_create(maps_screen);
        lv_obj_remove_style_all(maps_map_container);
        lv_obj_set_pos(maps_map_container, 0, 32);
        lv_obj_set_size(maps_map_container, 320, 208);
        lv_obj_set_style_bg_color(maps_map_container, lv_color_hex(0x111114), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(maps_map_container, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(maps_map_container, LV_OBJ_FLAG_SCROLLABLE);

        // long-press anywhere on the map drops a pin at that geographic spot
        lv_obj_add_flag(maps_map_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            maps_map_container,
            [](lv_event_t *) {
                lv_indev_t *indev = lv_indev_active();
                if (!indev || !THIS->userMap)
                    return;
                lv_point_t pt;
                lv_indev_get_point(indev, &pt);
                lv_area_t a;
                lv_obj_get_coords(THIS->maps_map_container, &a);
                float lat, lon;
                THIS->userMap->screenToGeo((int16_t)(pt.x - a.x1), (int16_t)(pt.y - a.y1), lat, lon);
                THIS->dropPinAt(lat, lon);
            },
            LV_EVENT_LONG_PRESSED, NULL);

        // swipe scrolls the map (same direction mapping as the mesh map's gestures)
        lv_obj_add_event_cb(
            maps_screen,
            [](lv_event_t *) {
                if (!THIS->userMap || !THIS->userMap->redrawComplete())
                    return;
                int16_t dx = 0, dy = 0;
                switch (lv_indev_get_gesture_dir(lv_indev_active())) {
                case LV_DIR_LEFT:
                    dx = -1;
                    break;
                case LV_DIR_RIGHT:
                    dx = 1;
                    break;
                case LV_DIR_TOP:
                    dy = -1;
                    break;
                case LV_DIR_BOTTOM:
                    dy = 1;
                    break;
                default:
                    return;
                }
                if (!THIS->userMap->scroll(dx, dy))
                    THIS->userMap->forceRedraw();
            },
            LV_EVENT_GESTURE, NULL);

        // ---- top bar: Back · − · + · Me · Pins ----
        auto barBtn = [this](const char *txt, int32_t w, lv_align_t align, int32_t xofs, uint32_t color,
                             lv_event_cb_t cb) -> lv_obj_t * {
            lv_obj_t *btn = lv_btn_create(maps_screen);
            lv_obj_set_size(btn, w, 24);
            lv_obj_align(btn, align, xofs, 0);
            lv_obj_set_y(btn, 4);
            lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, txt);
            lv_obj_center(lbl);
            return btn;
        };
        barBtn("Back", 56, LV_ALIGN_TOP_LEFT, 4, 0x2c2c2e, [](lv_event_t *) {
            THIS->closePinsList();
            if (THIS->launcher_screen)
                lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        });
        barBtn("-", 34, LV_ALIGN_TOP_LEFT, 66, 0x2c2c2e, [](lv_event_t *) {
            if (THIS->userMap) {
                THIS->userMap->setZoom(MapTileSettings::getZoomLevel() - 1);
                THIS->updateMapsZoom();
            }
        });
        barBtn("+", 34, LV_ALIGN_TOP_LEFT, 104, 0x2c2c2e, [](lv_event_t *) {
            if (THIS->userMap) {
                THIS->userMap->setZoom(MapTileSettings::getZoomLevel() + 1);
                THIS->updateMapsZoom();
            }
        });
        barBtn("Me", 44, LV_ALIGN_TOP_RIGHT, -66, 0x30d158, [](lv_event_t *) {
            if (!THIS->userMap)
                return;
            int32_t lat, lon;
            if (tdeck_gps_position(&lat, &lon)) { // freshest: straight from the GPS driver
                THIS->userMap->setScrolledPosition(lat * 1e-7, lon * 1e-7);
            } else if (THIS->hasPosition) { // fallback: last own-position packet the UI saw
                THIS->userMap->setScrolledPosition(THIS->myLatitude * 1e-7, THIS->myLongitude * 1e-7);
            } else { // no fix at all — say so instead of silently doing nothing
                char buf[40];
                snprintf(buf, sizeof(buf), "No GPS fix yet (%u sats)", (unsigned)tdeck_gps_num_sats());
                THIS->mapsShowNotice(buf);
            }
        });
        barBtn("Pins", 56, LV_ALIGN_TOP_RIGHT, -4, 0x0a84ff, [](lv_event_t *) { THIS->openPinsList(); });

        // satellite readout in the top-bar gap between "+" and "Me" (green = locked)
        maps_sats_label = lv_label_create(maps_screen);
        lv_label_set_text(maps_sats_label, "");
        lv_obj_set_style_text_color(maps_sats_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(maps_sats_label, LV_ALIGN_TOP_MID, 14, 9);
        maps_sats_timer = lv_timer_create([](lv_timer_t *) { THIS->updateMapsSats(); }, 1000, NULL);

        // gear cog, bottom-right over the map: style picker + region downloader
        maps_gear_btn = lv_btn_create(maps_screen);
        lv_obj_set_size(maps_gear_btn, 36, 36);
        lv_obj_set_style_radius(maps_gear_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(maps_gear_btn, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(maps_gear_btn, LV_OPA_80, LV_PART_MAIN);
        lv_obj_align(maps_gear_btn, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
        {
            // draw a mini gear, 8 teeth (4 cardinal + 4 diagonal), like a real cog —
            // this build's font has no symbol glyphs, so LV_SYMBOL_SETTINGS renders blank
            lv_obj_set_style_pad_all(maps_gear_btn, 0, LV_PART_MAIN);
            icRing(maps_gear_btn, 10, 10, 16, 0xffffff, 3);
            icBox(maps_gear_btn, 16, 5, 4, 6, 0xffffff, 1);  // top
            icBox(maps_gear_btn, 16, 25, 4, 6, 0xffffff, 1); // bottom
            icBox(maps_gear_btn, 5, 16, 6, 4, 0xffffff, 1);  // left
            icBox(maps_gear_btn, 25, 16, 6, 4, 0xffffff, 1); // right
            icBox(maps_gear_btn, 8, 8, 4, 4, 0xffffff, 1);   // diagonals
            icBox(maps_gear_btn, 24, 8, 4, 4, 0xffffff, 1);
            icBox(maps_gear_btn, 8, 24, 4, 4, 0xffffff, 1);
            icBox(maps_gear_btn, 24, 24, 4, 4, 0xffffff, 1);
        }
        lv_obj_add_event_cb(
            maps_gear_btn, [](lv_event_t *) { THIS->openMapsMenu(); }, LV_EVENT_CLICKED, NULL);

        // zoom level readout, bottom-left over the map
        maps_zoom_label = lv_label_create(maps_screen);
        lv_obj_set_style_text_color(maps_zoom_label, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_bg_color(maps_zoom_label, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(maps_zoom_label, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(maps_zoom_label, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(maps_zoom_label, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(maps_zoom_label, 6, LV_PART_MAIN);
        lv_obj_align(maps_zoom_label, LV_ALIGN_BOTTOM_LEFT, 6, -6);
        updateMapsZoom();

        // Leaving the Maps app? Drop its tile cache to free RAM; it rebuilds on return.
        lv_obj_add_event_cb(
            maps_screen,
            [](lv_event_t *) {
                THIS->closeMapsMenu();
                if (THIS->userMap)
                    THIS->userMap->releaseTiles();
            },
            LV_EVENT_SCREEN_UNLOADED, NULL);
    }

    lv_screen_load_anim(maps_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    updateMapsSats();

    if (!userMap) {
        mapsInitTileStyle();
#if LV_USE_FS_ARDUINO_SD
        userMap = new MapPanel(maps_map_container);
#elif defined(HAS_SD_MMC) || defined(HAS_SDCARD)
        ITileService *tileService = sharedTileService();
        userMap = new MapPanel(maps_map_container, tileService);
        userMap->setBackupService(
            new URLService([tileService](const char *name, void *img, size_t len) { return tileService->save(name, img, len); }));
#else
        userMap = new MapPanel(maps_map_container, sharedTileService());
#endif
        userMap->setNoTileImage(&img_no_tile_image);

        // "you are here" dot (our own marker; the mesh map keeps its own images)
        if (db.config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
            maps_gps_dot = lv_obj_create(maps_map_container);
            lv_obj_remove_style_all(maps_gps_dot);
            lv_obj_set_size(maps_gps_dot, 14, 14);
            lv_obj_set_style_radius(maps_gps_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(maps_gps_dot, lv_color_hex(0x0a84ff), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(maps_gps_dot, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(maps_gps_dot, lv_color_hex(0xffffff), LV_PART_MAIN);
            lv_obj_set_style_border_width(maps_gps_dot, 2, LV_PART_MAIN);
            lv_obj_add_flag(maps_gps_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(maps_gps_dot, LV_OBJ_FLAG_CLICKABLE);
            userMap->setGpsPositionImage(maps_gps_dot);
        }

        // center like the mesh map does: GPS > saved home > world view
        if (hasPosition) {
            if (db.uiConfig.map_data.has_home) {
                userMap->setHomeLocation(db.uiConfig.map_data.home.latitude * 1e-7, db.uiConfig.map_data.home.longitude * 1e-7);
                userMap->setZoom(db.uiConfig.map_data.home.zoom);
            } else {
                userMap->setHomeLocation(myLatitude * 1e-7, myLongitude * 1e-7);
                userMap->setZoom(13);
            }
            userMap->setGpsPosition(myLatitude * 1e-7, myLongitude * 1e-7);
        } else if (db.uiConfig.map_data.has_home) {
            userMap->setHomeLocation(db.uiConfig.map_data.home.latitude * 1e-7, db.uiConfig.map_data.home.longitude * 1e-7);
            userMap->setZoom(db.uiConfig.map_data.home.zoom);
        } else {
            userMap->setZoom(3);
        }

        loadPins();    // read saved pins from SD (once per boot)
        drawAllPins(); // attach them to our map
        // Tell the user what came back from the card (temporary, while we chase the bug):
        // "N pins loaded" vs "no saved pins found" pinpoints save-vs-load at a glance.
        {
            char nb[32];
            if (mapPins.empty())
                snprintf(nb, sizeof(nb), "no saved pins found");
            else
                snprintf(nb, sizeof(nb), "%u pins loaded", (unsigned)mapPins.size());
            mapsShowNotice(nb);
        }
    } else {
        // the mesh map may have moved the shared zoom / redraw statics since we
        // were last on screen; setZoom() re-centers and forces a full tile rebuild
        userMap->setZoom(MapTileSettings::getZoomLevel());
    }

    meshMapStale = true; // and the mesh map re-syncs the same way, next time it loads
}

// 1 Hz while the Maps screen is up: sats-in-view from the GPS driver; also expires
// the transient notice so it needs no timer of its own.
// Bottom-left zoom readout. Cheap enough to refresh from the 1s sats timer as a
// catch-all (style switches, Me-button re-centers...) plus instantly from the +/-.
void TFTView_320x240::updateMapsZoom(void)
{
    if (maps_zoom_label)
        lv_label_set_text_fmt(maps_zoom_label, "z%u", (unsigned)MapTileSettings::getZoomLevel());
}

void TFTView_320x240::updateMapsSats(void)
{
    updateMapsZoom();
    if (!maps_sats_label || lv_screen_active() != maps_screen)
        return;

    if (db.config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
        lv_label_set_text(maps_sats_label, "no GPS");
        lv_obj_set_style_text_color(maps_sats_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    } else if (db.config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_DISABLED) {
        lv_label_set_text(maps_sats_label, "GPS off");
        lv_obj_set_style_text_color(maps_sats_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    } else {
        bool lock = tdeck_gps_has_lock();
        uint32_t sats = tdeck_gps_num_sats();
        lv_label_set_text_fmt(maps_sats_label, "%u sats", (unsigned)sats);
        // Green only when the fix is trustworthy. A 3-sat "lock" (or bad satellite
        // geometry, PDOP > 5) is a marginal 2D fix that can sit blocks away — show
        // it AMBER so a wandering dot reads as "weak signal", not a broken map.
        uint32_t dop = tdeck_gps_dop();
        bool weak = sats < 4 || dop > 500;
        lv_obj_set_style_text_color(maps_sats_label, lv_color_hex(!lock ? 0x8e8e93 : (weak ? 0xff9f0a : 0x30d158)),
                                    LV_PART_MAIN);
        // Keep the "you are here" dot on the RAW GPS fix — the same source the "Me"
        // button uses — instead of the mesh node position, which Meshtastic snaps to
        // a privacy grid (that mismatch is why the dot sat in the wrong spot).
        if (lock && userMap) {
            int32_t glat = 0, glon = 0;
            if (tdeck_gps_position(&glat, &glon))
                userMap->setGpsPosition(glat * 1e-7, glon * 1e-7);
        }
    }
    lv_obj_align(maps_sats_label, LV_ALIGN_TOP_MID, 14, 9); // re-center after text change

    if (maps_notice && maps_notice_until && lv_tick_get() > maps_notice_until) {
        lv_obj_add_flag(maps_notice, LV_OBJ_FLAG_HIDDEN);
        maps_notice_until = 0;
    }
}

// Transient message over the map ("No GPS fix yet (3 sats)"); updateMapsSats hides it.
void TFTView_320x240::mapsShowNotice(const char *msg)
{
    if (!maps_screen)
        return;
    if (!maps_notice) {
        maps_notice = lv_label_create(maps_screen);
        lv_obj_set_style_bg_color(maps_notice, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(maps_notice, LV_OPA_90, LV_PART_MAIN);
        lv_obj_set_style_radius(maps_notice, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(maps_notice, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(maps_notice, 6, LV_PART_MAIN);
        lv_obj_set_style_text_color(maps_notice, lv_color_hex(0xffffff), LV_PART_MAIN);
    }
    lv_label_set_text(maps_notice, msg);
    lv_obj_align(maps_notice, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_clear_flag(maps_notice, LV_OBJ_FLAG_HIDDEN);
    maps_notice_until = lv_tick_get() + 2500;
}

// ===== Maps app: style picker (gear) + USGS region downloader =============================
namespace {
// slippy-map tile math (float is plenty at our zooms)
inline float lon2tx(float lon, uint8_t z) { return (lon + 180.0f) / 360.0f * exp2f(z); }
inline float lat2ty(float lat, uint8_t z)
{
    float r = lat * (float)M_PI / 180.0f;
    return (1.0f - asinhf(tanf(r)) / (float)M_PI) / 2.0f * exp2f(z);
}
inline float tx2lon(float x, uint8_t z) { return x / exp2f(z) * 360.0f - 180.0f; }
inline float ty2lat(float y, uint8_t z)
{
    float v = (float)M_PI * (1.0f - 2.0f * y / exp2f(z));
    return atanf(sinhf(v)) * 180.0f / (float)M_PI;
}
// x/y tile index ranges of a lat/lon box at zoom z (clamped to the world)
inline void boxTiles(float latN, float latS, float lonW, float lonE, uint8_t z, uint32_t &x0, uint32_t &x1, uint32_t &y0,
                     uint32_t &y1)
{
    float n = exp2f(z);
    auto clampf = [&](float v) { return v < 0 ? 0.0f : (v >= n ? n - 1 : v); };
    x0 = (uint32_t)clampf(floorf(lon2tx(lonW, z)));
    x1 = (uint32_t)clampf(floorf(lon2tx(lonE, z)));
    y0 = (uint32_t)clampf(floorf(lat2ty(latN, z)));
    y1 = (uint32_t)clampf(floorf(lat2ty(latS, z)));
}

// Downloadable tile sources. Only services whose terms allow bulk downloads make this
// list (government mapping agencies, basically). The template is {z}/{y}/{x} form —
// mapdlBuildUrl fills it — and doubles as the .url browse-fill line written to the
// card. estKB feeds the size/time estimate (USGS jpg ≈ 20KB, TopPlus png ≈ 35KB).
struct MapDlSource {
    const char *label;  // shown in the Source dropdown, region-prefixed
    const char *folder; // style folder under /maps
    const char *urlTemplate;
    const char *ext; // tile format written to .format ("jpg"/"png")
    uint8_t estKB;   // typical tile size for the estimate
};
const MapDlSource kMapDlSources[] = {
    {"(US) USGS Topo", "USGS-Topo", "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}",
     "jpg", 20},
    {"(EU) TopPlusOpen", "TopPlusOpen",
     "https://sgx.geodatenzentrum.de/wmts_topplus_open/tile/1.0.0/web/default/WEBMERCATOR/{z}/{y}/{x}.png", "png", 35},
};
// Detail levels offered on the download screen. Both dropdowns share this list; the value is
// the index + 1, so the last entry here is the deepest zoom the downloader will fetch.
const char *kMapDlDetailOpts = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18";

int s_mapdlSrcIdx = 0; // which source the download screen has selected
inline const MapDlSource &mapdlSrc(void)
{
    return kMapDlSources[s_mapdlSrcIdx];
}

// Fill a {z}/{y}/{x} template with real numbers.
void mapdlBuildUrl(char *out, size_t cap, const char *tpl, uint8_t z, uint32_t x, uint32_t y)
{
    size_t o = 0;
    for (const char *p = tpl; *p && o + 12 < cap; p++) {
        if (p[0] == '{' && p[2] == '}') {
            unsigned long v = (p[1] == 'z') ? z : (p[1] == 'y') ? y : x;
            o += snprintf(out + o, cap - o, "%lu", v);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

#ifdef ARDUINO_ARCH_ESP32
// One TLS connection reused for the whole download (a handshake per tile would be
// brutally slow and heap-hungry). Created on start, freed on stop — TLS holds ~45KB
// of heap while alive, which the mesh needs back afterwards.
WiFiClientSecure *mapdlClient = nullptr;

void mapdlTilePath(char *buf, size_t cap, uint8_t z, uint32_t x, uint32_t y)
{
    snprintf(buf, cap, "%s/%s/%u/%lu/%lu.%s", MapTileSettings::getPrefix(), mapdlSrc().folder, (unsigned)z, (unsigned long)x,
             (unsigned long)y, mapdlSrc().ext);
}

bool mapdlFetch(uint8_t z, uint32_t x, uint32_t y)
{
    if (!mapdlClient) {
        mapdlClient = new WiFiClientSecure();
        mapdlClient->setInsecure(); // public map data; no room for a CA bundle
    }
    char url[160];
    mapdlBuildUrl(url, sizeof(url), mapdlSrc().urlTemplate, z, x, y);
    HTTPClient http;
    http.setReuse(true); // keep-alive on the shared client
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(*mapdlClient, url))
        return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }
    int len = http.getSize();
    if (len <= 0 || len > 262144) {
        http.end();
        return false;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    int got = 0;
    uint8_t idle = 0;
    while (got < len) {
        size_t avail = stream->available();
        if (!avail) {
            if (++idle > 100) // ~1s of silence mid-body = give up on this tile
                break;
            delay(10);
            continue;
        }
        idle = 0;
        size_t want = len - got;
        if (want > avail)
            want = avail;
        int r = stream->read(buf + got, want);
        if (r <= 0)
            break;
        got += r;
    }
    http.end();
    bool ok = (got == len);
    if (ok) {
        char dir[104], path[120];
        snprintf(dir, sizeof(dir), "%s/%s/%u/%lu", MapTileSettings::getPrefix(), mapdlSrc().folder, (unsigned)z,
                 (unsigned long)x);
        SDFs.mkdir(dir); // SdFat creates missing parents
        mapdlTilePath(path, sizeof(path), z, x, y);
        FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
        ok = false;
        if (f) {
            ok = (f.write(buf, (size_t)len) == (size_t)len);
            f.close();
        }
    }
    heap_caps_free(buf);
    return ok;
}

// Make the USGS style folder self-describing so the style picker (and the mesh map's
// picker) can wire it up from the card alone: .url = browse-fill server, .format = jpg.
void mapdlWriteMeta(void)
{
    char p[104];
    snprintf(p, sizeof(p), "%s/%s", MapTileSettings::getPrefix(), mapdlSrc().folder);
    SDFs.mkdir(p);
    snprintf(p, sizeof(p), "%s/%s/.url", MapTileSettings::getPrefix(), mapdlSrc().folder);
    FsFile f = SDFs.open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.print(mapdlSrc().urlTemplate);
        f.print("\n");
        f.close();
    }
    snprintf(p, sizeof(p), "%s/%s/.format", MapTileSettings::getPrefix(), mapdlSrc().folder);
    f = SDFs.open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.print(mapdlSrc().ext);
        f.print("\n");
        f.close();
    }
}
#endif // ARDUINO_ARCH_ESP32
} // namespace

// Switch the Maps app to a tile style (a folder under /maps). Reads the style's
// optional .format (jpg/png, default png) and .url (browse-fill server) files, so a
// downloaded style Just Works. persist=true remembers it across reboots.
void TFTView_320x240::mapsApplyStyle(const char *style, bool persist)
{
    MapTileSettings::setTileStyle(style);

    char fmt[8] = "png";
    {
        char path[104];
        snprintf(path, sizeof(path), "%s/%s/.format", MapTileSettings::getPrefix(), style);
        FsFile f = SDFs.open(path, O_RDONLY);
        if (f) {
            int n = f.read((uint8_t *)fmt, sizeof(fmt) - 1);
            if (n > 0) {
                fmt[n] = 0;
                for (char *c = fmt; *c; c++) {
                    if (*c == '\r' || *c == '\n') {
                        *c = 0;
                        break;
                    }
                }
            }
            f.close();
        }
    }
    if (!fmt[0])
        strcpy(fmt, "png");
    MapTileSettings::setTileFormat(fmt);

    // browse-fill: point the backup fetcher at this style's own server, exactly like
    // the mesh map's style dropdown does. No .url file = no fetching, no SD writes.
    std::string url = sdCard ? sdCard->getUrlProvider(MapTileSettings::getPrefix(), style) : std::string{};
    if (!url.empty()) {
        std::string provider = std::string("URL: ") + style;
        int entry = TileProvider::addTemplate(provider, url);
        TileProvider::selectTemplate(entry);
    }
    MapTileSettings::setSaveOK(!url.empty());

    if (persist) {
        strncpy(db.uiConfig.map_data.style, style, sizeof(db.uiConfig.map_data.style) - 1);
        db.uiConfig.map_data.style[sizeof(db.uiConfig.map_data.style) - 1] = 0;
        db.uiConfig.has_map_data = true;
        controller->storeUIConfig(db.uiConfig);
    }

    if (userMap)
        userMap->setZoom(MapTileSettings::getZoomLevel()); // re-center + full tile rebuild
    meshMapStale = true; // the mesh map shares this state; it re-syncs on next load
}

void TFTView_320x240::closeMapsMenu(void)
{
    if (maps_style_ovl) {
        lv_obj_delete(maps_style_ovl);
        maps_style_ovl = nullptr;
    }
}

// The gear menu: pick a map style (folders under /maps) or open the region downloader.
void TFTView_320x240::openMapsMenu(void)
{
    if (!maps_screen || maps_style_ovl)
        return;
    maps_style_ovl = lv_obj_create(maps_screen);
    lv_obj_set_size(maps_style_ovl, 250, 206);
    lv_obj_center(maps_style_ovl);
    lv_obj_set_style_bg_color(maps_style_ovl, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_color(maps_style_ovl, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_radius(maps_style_ovl, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(maps_style_ovl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(maps_style_ovl, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(maps_style_ovl, 6, LV_PART_MAIN);

    auto row = [&](const char *txt, uint32_t color, lv_event_cb_t cb, void *ud) {
        lv_obj_t *b = lv_btn_create(maps_style_ovl);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, 36);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
        return b;
    };

    lv_obj_t *title = lv_label_create(maps_style_ovl);
    lv_label_set_text(title, "Map style");
    lv_obj_set_style_text_color(title, lv_color_hex(0x8e8e93), LV_PART_MAIN);

    // one row per style folder on the card; checkmark = the active one
    static char styleNames[6][24]; // callback user_data must outlive this function
    int i = 0;
    if (sdCard) {
        std::set<std::string> styles = sdCard->loadMapStyles(MapTileSettings::getPrefix());
        for (auto &s : styles) {
            if (i >= 6)
                break;
            strncpy(styleNames[i], s.c_str(), sizeof(styleNames[0]) - 1);
            styleNames[i][sizeof(styleNames[0]) - 1] = 0;
            char withSlash[26];
            snprintf(withSlash, sizeof(withSlash), "%s/", styleNames[i]);
            bool current = (strcmp(withSlash, MapTileSettings::getTileStyle()) == 0);
            // known downloadable sources get a region prefix so people know coverage
            const char *region = "";
            for (auto &s : kMapDlSources) {
                if (strcmp(styleNames[i], s.folder) == 0) {
                    region = s.label; // starts with "(US) " / "(EU) " — use just that bit
                    break;
                }
            }
            char pre[8] = "";
            if (region[0] == '(') {
                const char *close = strchr(region, ')');
                if (close && (size_t)(close - region) + 2 < sizeof(pre)) {
                    memcpy(pre, region, close - region + 1);
                    pre[close - region + 1] = ' ';
                    pre[close - region + 2] = 0;
                }
            }
            char lbl[44];
            snprintf(lbl, sizeof(lbl), "%s%s%s", current ? LV_SYMBOL_OK " " : "", pre, styleNames[i]);
            row(lbl, 0xffffff,
                [](lv_event_t *e) {
                    const char *style = (const char *)lv_event_get_user_data(e);
                    THIS->closeMapsMenu();
                    THIS->mapsApplyStyle(style, true);
                    char msg[40];
                    snprintf(msg, sizeof(msg), "map: %s", style);
                    THIS->mapsShowNotice(msg);
                },
                styleNames[i]);
            i++;
        }
    }
    if (i == 0) {
        lv_obj_t *none = lv_label_create(maps_style_ovl);
        lv_label_set_text(none, "no styles found on card");
        lv_obj_set_style_text_color(none, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    }

    row(mapdl_running ? "Download progress..." : "Download this area...", 0x30d158,
        [](lv_event_t *) {
            THIS->closeMapsMenu();
            THIS->openMapDownload();
        },
        NULL);
    row("Close", 0x8e8e93, [](lv_event_t *) { THIS->closeMapsMenu(); }, NULL);
}

uint32_t TFTView_320x240::mapdlCountTiles(uint8_t zmin, uint8_t zmax) const
{
    uint32_t total = 0;
    for (uint8_t z = zmin; z <= zmax; z++) {
        uint32_t x0, x1, y0, y1;
        boxTiles(mapdl_latN, mapdl_latS, mapdl_lonW, mapdl_lonE, z, x0, x1, y0, y1);
        total += (x1 - x0 + 1) * (y1 - y0 + 1);
    }
    return total;
}

void TFTView_320x240::mapdlUpdateEstimate(void)
{
    if (!mapdl_screen || mapdl_running)
        return;
    mapdl_zmin = 1 + (uint8_t)lv_dropdown_get_selected(mapdl_zmin_dd);
    mapdl_zmax = 1 + (uint8_t)lv_dropdown_get_selected(mapdl_zmax_dd);
    if (mapdl_zmax < mapdl_zmin) { // keep the range sane instead of erroring
        mapdl_zmax = mapdl_zmin;
        lv_dropdown_set_selected(mapdl_zmax_dd, mapdl_zmax - 1);
    }
    uint32_t tiles = mapdlCountTiles(mapdl_zmin, mapdl_zmax);
    uint32_t mb10 = tiles * mapdlSrc().estKB / 102; // per-source typical KB/tile, shown in tenths of MB
    uint32_t mins = tiles / 180 + 1;                // ~3 tiles/s
    char buf[96];
    snprintf(buf, sizeof(buf), "%lu tiles  ~%lu.%lu MB  ~%lu min", (unsigned long)tiles, (unsigned long)(mb10 / 10),
             (unsigned long)(mb10 % 10), (unsigned long)mins);
    lv_label_set_text(mapdl_status, buf);
}

// The download-area screen. The box is whatever the map was showing when this opened.
void TFTView_320x240::openMapDownload(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!mapdl_running && userMap && maps_map_container) {
        float clat = 0, clon = 0;
        userMap->getCenter(clat, clon);
        uint8_t z = MapTileSettings::getZoomLevel();
        float xt = lon2tx(clon, z), yt = lat2ty(clat, z);
        float halfX = lv_obj_get_width(maps_map_container) / 2.0f / 256.0f;
        float halfY = lv_obj_get_height(maps_map_container) / 2.0f / 256.0f;
        mapdl_lonW = tx2lon(xt - halfX, z);
        mapdl_lonE = tx2lon(xt + halfX, z);
        mapdl_latN = ty2lat(yt - halfY, z);
        mapdl_latS = ty2lat(yt + halfY, z);
    }

    if (!mapdl_screen) {
        mapdl_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(mapdl_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(mapdl_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(mapdl_screen);
        lv_label_set_text(title, "Download map area");
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        mapdl_status = lv_label_create(mapdl_screen);
        // wrap + center so a long progress line stays on screen instead of running
        // off both edges
        lv_obj_set_width(mapdl_status, 296);
        lv_label_set_long_mode(mapdl_status, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(mapdl_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(mapdl_status, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_align(mapdl_status, LV_ALIGN_TOP_MID, 0, 26);
        lv_label_set_text(mapdl_status, "");

        // NOTE this screen is a fixed 320x240 budget: title 4-25, status 26-72 (2 lines
        // max), info 76-116 (two SHORT lines — this font fits ~26 chars in 300px and a
        // wrapped third line slides under the rows below; keep every info string short),
        // Source row 120-155, Detail row 158-193, buttons 196-234.
        mapdl_info = lv_label_create(mapdl_screen);
        lv_obj_set_width(mapdl_info, 300);
        lv_label_set_long_mode(mapdl_info, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(mapdl_info, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(mapdl_info, LV_ALIGN_TOP_LEFT, 12, 76);
        lv_label_set_text(mapdl_info, "Area = what the map shows.\nSleep is OK once started.");

        lv_obj_t *ls = lv_label_create(mapdl_screen);
        lv_label_set_text(ls, "Source:");
        lv_obj_set_style_text_color(ls, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(ls, LV_ALIGN_TOP_LEFT, 12, 128);

        mapdl_src_dd = lv_dropdown_create(mapdl_screen);
        {
            std::string opts;
            for (size_t i = 0; i < sizeof(kMapDlSources) / sizeof(kMapDlSources[0]); i++) {
                if (i)
                    opts += "\n";
                opts += kMapDlSources[i].label;
            }
            lv_dropdown_set_options(mapdl_src_dd, opts.c_str());
        }
        lv_dropdown_set_selected(mapdl_src_dd, (uint32_t)s_mapdlSrcIdx);
        lv_obj_set_width(mapdl_src_dd, 210);
        lv_obj_align(mapdl_src_dd, LV_ALIGN_TOP_LEFT, 98, 120);
        lv_obj_add_event_cb(
            mapdl_src_dd,
            [](lv_event_t *e) {
                s_mapdlSrcIdx = (int)lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
                THIS->mapdlUpdateEstimate();
            },
            LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *l1 = lv_label_create(mapdl_screen);
        lv_label_set_text(l1, "Detail:");
        lv_obj_set_style_text_color(l1, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 12, 166);

        mapdl_zmin_dd = lv_dropdown_create(mapdl_screen);
        // 1-15: low zooms are nearly free (a handful of tiles) and give the zoomed-out
        // view, so the default range includes them all
        lv_dropdown_set_options(mapdl_zmin_dd, kMapDlDetailOpts);
        lv_dropdown_set_selected(mapdl_zmin_dd, 0);
        lv_obj_set_width(mapdl_zmin_dd, 66);
        lv_obj_align(mapdl_zmin_dd, LV_ALIGN_TOP_LEFT, 98, 158);
        lv_obj_add_event_cb(
            mapdl_zmin_dd, [](lv_event_t *) { THIS->mapdlUpdateEstimate(); }, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *l2 = lv_label_create(mapdl_screen);
        lv_label_set_text(l2, "to");
        lv_obj_set_style_text_color(l2, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 172, 166);

        mapdl_zmax_dd = lv_dropdown_create(mapdl_screen);
        lv_dropdown_set_options(mapdl_zmax_dd, kMapDlDetailOpts);
        // Default stays at 15. Each extra level is ~4x the tiles, so 18 is ~64x a 15 -
        // it's there when you want a small area in close detail, not as a default.
        lv_dropdown_set_selected(mapdl_zmax_dd, 14);
        lv_obj_set_width(mapdl_zmax_dd, 66);
        lv_obj_align(mapdl_zmax_dd, LV_ALIGN_TOP_LEFT, 198, 158);
        lv_obj_add_event_cb(
            mapdl_zmax_dd, [](lv_event_t *) { THIS->mapdlUpdateEstimate(); }, LV_EVENT_VALUE_CHANGED, NULL);

        mapdl_btn = lv_btn_create(mapdl_screen);
        lv_obj_set_size(mapdl_btn, 90, 38);
        lv_obj_set_style_bg_color(mapdl_btn, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_align(mapdl_btn, LV_ALIGN_BOTTOM_LEFT, 12, -6);
        mapdl_btn_lbl = lv_label_create(mapdl_btn);
        lv_label_set_text(mapdl_btn_lbl, "Start");
        lv_obj_set_style_text_color(mapdl_btn_lbl, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_center(mapdl_btn_lbl);
        lv_obj_add_event_cb(
            mapdl_btn,
            [](lv_event_t *) {
                if (THIS->mapdl_running)
                    THIS->mapdlStop(false);
                else
                    THIS->mapdlStart();
            },
            LV_EVENT_CLICKED, NULL);

        mapdl_use_btn = lv_btn_create(mapdl_screen);
        // 90+120+70 wide + margins: all three bottom buttons coexist without overlap
        lv_obj_set_size(mapdl_use_btn, 120, 38);
        lv_obj_set_style_bg_color(mapdl_use_btn, lv_color_hex(0x0a84ff), LV_PART_MAIN);
        lv_obj_align(mapdl_use_btn, LV_ALIGN_BOTTOM_LEFT, 110, -6);
        lv_obj_t *ubl = lv_label_create(mapdl_use_btn);
        lv_label_set_text(ubl, "Use this map");
        lv_obj_set_style_text_color(ubl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_center(ubl);
        lv_obj_add_flag(mapdl_use_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(
            mapdl_use_btn,
            [](lv_event_t *) {
                THIS->mapsApplyStyle(mapdlSrc().folder, true);
                THIS->openMaps();
            },
            LV_EVENT_CLICKED, NULL);

        lv_obj_t *back = lv_btn_create(mapdl_screen);
        lv_obj_set_size(back, 70, 38);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, "Back");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_center(bl);
        // Back just leaves the screen — a running download keeps going headless.
        lv_obj_add_event_cb(
            back, [](lv_event_t *) { THIS->openMaps(); }, LV_EVENT_CLICKED, NULL);
    }

    lv_screen_load_anim(mapdl_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    if (mapdl_running) {
        lv_label_set_text(mapdl_btn_lbl, "Stop");
        lv_obj_add_flag(mapdl_use_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(mapdl_btn_lbl, "Start");
        mapdlUpdateEstimate();
    }
#endif // ARDUINO_ARCH_ESP32
}

void TFTView_320x240::mapdlStart(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (mapdl_running)
        return;
    mapdl_zmin = 1 + (uint8_t)lv_dropdown_get_selected(mapdl_zmin_dd);
    mapdl_zmax = 1 + (uint8_t)lv_dropdown_get_selected(mapdl_zmax_dd);
    if (mapdl_zmax < mapdl_zmin)
        mapdl_zmax = mapdl_zmin;
    mapdl_total = mapdlCountTiles(mapdl_zmin, mapdl_zmax);
    mapdl_done = mapdl_failed = mapdl_skipped = 0;
    mapdl_z = mapdl_zmin;
    uint32_t x0, x1, y0, y1;
    boxTiles(mapdl_latN, mapdl_latS, mapdl_lonW, mapdl_lonE, mapdl_z, x0, x1, y0, y1);
    mapdl_x = x0;
    mapdl_y = y0;

    // WiFi: use it if it's already up, otherwise bring it up on-demand (fileshare pattern)
    bool already = tdeck_wifi_connected();
    mapdl_own_wifi = !already;
    mapdl_wifi_up = already;
    if (!already && !tdeck_wifi_connect_now(db.config.network.wifi_ssid, db.config.network.wifi_psk)) {
        lv_label_set_text(mapdl_status, "No Wi-Fi network saved");
        lv_label_set_text(mapdl_info, "Set one in Settings -> Wi-Fi,\nthen come back here.");
        return;
    }
    mapdl_deadline = lv_tick_get() + 25000;
    mapdl_running = true;
    // Two SHORT lines only — see the layout note in openMapDownload. Leaving this screen
    // currently stops the download (Jake hit this on 16.7); sleeping is safe, so say both.
    lv_label_set_text(mapdl_info, "Stay on this screen while\ndownloading. Sleep is OK.");
    lv_label_set_text(mapdl_btn_lbl, "Stop");
    lv_obj_add_flag(mapdl_use_btn, LV_OBJ_FLAG_HIDDEN);
    // lock the knobs while running — the cursor math depends on them
    if (mapdl_src_dd)
        lv_obj_add_state(mapdl_src_dd, LV_STATE_DISABLED);
    if (mapdl_zmin_dd)
        lv_obj_add_state(mapdl_zmin_dd, LV_STATE_DISABLED);
    if (mapdl_zmax_dd)
        lv_obj_add_state(mapdl_zmax_dd, LV_STATE_DISABLED);
    if (!already)
        lv_label_set_text(mapdl_status, "Turning Wi-Fi on...");

    mapdlWriteMeta(); // style folder is self-describing even if we stop early

    if (!mapdl_timer)
        mapdl_timer = lv_timer_create([](lv_timer_t *) { THIS->mapdlPump(); }, 50, NULL);
#endif
}

void TFTView_320x240::mapdlStop(bool finished)
{
#ifdef ARDUINO_ARCH_ESP32
    if (mapdl_timer) {
        lv_timer_delete(mapdl_timer);
        mapdl_timer = nullptr;
    }
    if (mapdlClient) { // free the TLS heap (~45KB) the moment we're done with it
        delete mapdlClient;
        mapdlClient = nullptr;
    }
    if (mapdl_own_wifi && mapdl_wifi_up)
        tdeck_wifi_disconnect_now();
    if (mapdl_own_wifi && !mapdl_wifi_up)
        tdeck_wifi_disconnect_now(); // half-connected attempt: tear it down too
    mapdl_running = false;
    mapdl_wifi_up = false;
    if (mapdl_screen) {
        lv_label_set_text(mapdl_btn_lbl, "Start");
        if (mapdl_src_dd)
            lv_obj_remove_state(mapdl_src_dd, LV_STATE_DISABLED);
        if (mapdl_zmin_dd)
            lv_obj_remove_state(mapdl_zmin_dd, LV_STATE_DISABLED);
        if (mapdl_zmax_dd)
            lv_obj_remove_state(mapdl_zmax_dd, LV_STATE_DISABLED);
        char buf[96];
        if (finished)
            snprintf(buf, sizeof(buf), "Done! %lu new, %lu had, %lu failed", (unsigned long)mapdl_done,
                     (unsigned long)mapdl_skipped, (unsigned long)mapdl_failed);
        else
            snprintf(buf, sizeof(buf), "Stopped at %lu of %lu tiles", (unsigned long)(mapdl_done + mapdl_skipped),
                     (unsigned long)mapdl_total);
        lv_label_set_text(mapdl_status, buf);
        // short lines only — see the layout note in openMapDownload
        lv_label_set_text(mapdl_info, finished ? "Saved to the card.\nTap \"Use this map\"." : "Progress is saved.\nStart continues it.");
        lv_obj_clear_flag(mapdl_use_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (finished)
        playBeep();
#endif
}

// One pump tick: advance WiFi bring-up, then fetch AT MOST one tile (each HTTP round
// trip blocks the UI task briefly; one per tick keeps touch/redraw usable). Runs on
// the LVGL task on purpose: ALL SdFat access stays on one task.
void TFTView_320x240::mapdlPump(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!mapdl_running)
        return;

    if (!mapdl_wifi_up) {
        if (tdeck_wifi_connected()) {
            mapdl_wifi_up = true;
            if (lv_screen_active() == mapdl_screen)
                lv_label_set_text(mapdl_status, "Connected, downloading...");
        } else if (lv_tick_get() > mapdl_deadline) {
            if (mapdl_screen) {
                lv_label_set_text(mapdl_status, "Couldn't connect to Wi-Fi");
                lv_label_set_text(mapdl_info, "Check name + password in\nSettings -> Wi-Fi, then retry.");
            }
            mapdlStop(false);
        }
        return;
    }

    // each fetch blocks the UI task for its duration — while the user is actively
    // touching (dragging a map, playing a game), sit this tick out; idle ticks do
    // the fetching. Costs a little download speed only while the screen is in use.
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
            return;
    }

    uint32_t x0, x1, y0, y1;
    boxTiles(mapdl_latN, mapdl_latS, mapdl_lonW, mapdl_lonE, mapdl_z, x0, x1, y0, y1);

    // skip already-downloaded tiles fast (up to 32 per tick), fetch at most one
    for (int step = 0; step < 32; step++) {
        if (mapdl_y > y1) { // advance the cursor: y fastest, then x, then zoom
            mapdl_y = y0;
            mapdl_x++;
        }
        if (mapdl_x > x1) {
            if (mapdl_z >= mapdl_zmax) {
                mapdlStop(true);
                return;
            }
            mapdl_z++;
            boxTiles(mapdl_latN, mapdl_latS, mapdl_lonW, mapdl_lonE, mapdl_z, x0, x1, y0, y1);
            mapdl_x = x0;
            mapdl_y = y0;
        }

        char path[120];
        mapdlTilePath(path, sizeof(path), mapdl_z, mapdl_x, mapdl_y);
        if (SDFs.exists(path)) {
            mapdl_skipped++;
            mapdl_y++;
            continue;
        }
        if (mapdlFetch(mapdl_z, mapdl_x, mapdl_y))
            mapdl_done++;
        else
            mapdl_failed++;
        mapdl_y++;
        break; // one HTTP fetch per tick
    }

    if (mapdl_screen && lv_screen_active() == mapdl_screen) {
        char buf[96];
        uint32_t seen = mapdl_done + mapdl_skipped + mapdl_failed;
        uint32_t left = (mapdl_total > seen) ? (mapdl_total - seen) : 0;
        snprintf(buf, sizeof(buf), "%lu / %lu tiles  (~%lu min left)", (unsigned long)seen, (unsigned long)mapdl_total,
                 (unsigned long)(left / 180 + 1));
        lv_label_set_text(mapdl_status, buf);
    }
#endif
}

// ===== Get Apps: install add-on apps from the web, over Wi-Fi ============================
//
// The catalog published at jeeab.github.io/t-ui/apps/catalog.json is the single source of
// truth (the web page renders from the same file). An app is one small main.lua, so
// installing is: fetch the catalog -> fetch one text file -> write /apps/<id>/main.lua ->
// rescan. No reboot: scanUserApps() + rebuildAppGrid() make the new tile appear live.
#ifdef ARDUINO_ARCH_ESP32
namespace {
const char *kGetAppsCatalogUrl = "https://jeeab.github.io/t-ui/apps/catalog.json";
const char *kGetAppsBaseUrl = "https://jeeab.github.io/t-ui/";
WiFiClientSecure *getappsClient = nullptr;

// Download a whole (small) file into PSRAM. Returns nullptr on any failure; the caller
// frees with heap_caps_free. Timeouts are short on purpose: this runs on the UI task, so
// a stalled server must fail fast rather than freeze the screen the way map tiles used to.
uint8_t *getappsGet(const char *url, int *outLen)
{
    if (!getappsClient) {
        getappsClient = new WiFiClientSecure();
        getappsClient->setInsecure(); // public, read-only content; no room for a CA bundle
    }
    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    if (!http.begin(*getappsClient, url))
        return nullptr;
    if (http.GET() != HTTP_CODE_OK) {
        http.end();
        return nullptr;
    }
    int len = http.getSize();
    if (len <= 0 || len > 65536) { // a catalog or an app; anything bigger is wrong
        http.end();
        return nullptr;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        http.end();
        return nullptr;
    }
    WiFiClient *stream = http.getStreamPtr();
    int got = 0;
    uint16_t idle = 0;
    while (got < len) {
        size_t avail = stream->available();
        if (!avail) {
            if (++idle > 200) // ~2s of silence mid-body = give up
                break;
            delay(10);
            continue;
        }
        idle = 0;
        size_t want = (size_t)(len - got);
        if (want > avail)
            want = avail;
        int r = stream->read(buf + got, want);
        if (r <= 0)
            break;
        got += r;
    }
    http.end();
    if (got != len) {
        heap_caps_free(buf);
        return nullptr;
    }
    buf[len] = 0; // the parser below is string-based
    *outLen = len;
    return buf;
}

// Minimal readers for the catalog. We publish that file ourselves and the PR check
// validates its shape, so this only has to handle the JSON we generate - not arbitrary
// JSON. Everything is bounds-checked against `end` so a malformed file can't run away.
bool jsonStrField(const char *obj, const char *end, const char *key, char *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end)
        return false;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end)
        return false;
    while (p < end && *p != '"') { // skip to the opening quote of the value
        if (*p == ',' || *p == '}')
            return false;
        p++;
    }
    if (p >= end)
        return false;
    p++;
    size_t n = 0;
    while (p < end && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p + 1 < end)
            p++; // take the escaped character literally; our text has no fancy escapes
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

uint32_t jsonIntField(const char *obj, const char *end, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end)
        return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p || p >= end)
        return 0;
    p++;
    while (p < end && *p == ' ')
        p++;
    return (uint32_t)strtoul(p, nullptr, 10);
}
} // namespace
#endif // ARDUINO_ARCH_ESP32

void TFTView_320x240::openGetAppsAction(void)
{
    instance()->openGetApps();
}

bool TFTView_320x240::getappsIsInstalled(const char *id) const
{
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    char path[64];
    snprintf(path, sizeof(path), "/apps/%s/main.lua", id);
    return sdCard && SDFs.exists(path);
#else
    (void)id;
    return false;
#endif
}

// Parse catalog.json into storeApps[]. Returns the number of apps found.
int TFTView_320x240::getappsParseCatalog(const char *json, int len)
{
    storeAppCount = 0;
#ifdef ARDUINO_ARCH_ESP32
    const char *end = json + len;
    const char *arr = strstr(json, "\"apps\"");
    if (!arr)
        return 0;
    const char *p = strchr(arr, '[');
    if (!p)
        return 0;
    while (p < end && storeAppCount < kMaxStoreApps) {
        const char *ob = strchr(p, '{');
        if (!ob || ob >= end)
            break;
        const char *oe = strchr(ob, '}');
        if (!oe || oe >= end)
            break;
        StoreApp &a = storeApps[storeAppCount];
        // id + name are the two we can't do without; skip anything malformed rather than
        // showing the user a half-built row.
        if (jsonStrField(ob, oe, "id", a.id, sizeof(a.id)) && jsonStrField(ob, oe, "name", a.name, sizeof(a.name))) {
            if (!jsonStrField(ob, oe, "desc", a.desc, sizeof(a.desc)))
                a.desc[0] = 0;
            a.bytes = jsonIntField(ob, oe, "bytes");
            a.installed = getappsIsInstalled(a.id);
            storeAppCount++;
        }
        p = oe + 1;
    }
#else
    (void)json;
    (void)len;
#endif
    return storeAppCount;
}

// Download one app's main.lua and write it to /apps/<id>/main.lua.
bool TFTView_320x240::getappsInstall(int idx)
{
#if defined(ARDUINO_ARCH_ESP32) && HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    if (idx < 0 || idx >= storeAppCount || !sdCard)
        return false;
    StoreApp &a = storeApps[idx];
    char url[160];
    snprintf(url, sizeof(url), "%sapps/%s/main.lua", kGetAppsBaseUrl, a.id);
    int len = 0;
    uint8_t *buf = getappsGet(url, &len);
    if (!buf)
        return false;

    char dir[64], path[80];
    snprintf(dir, sizeof(dir), "/apps/%s", a.id);
    snprintf(path, sizeof(path), "/apps/%s/main.lua", a.id);
    SDFs.mkdir("/apps"); // no-op if it's already there
    SDFs.mkdir(dir);
    bool ok = false;
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        ok = (f.write(buf, (size_t)len) == (size_t)len);
        f.close();
    }
    heap_caps_free(buf);
    if (ok) {
        a.installed = true;
        // Make the tile appear right now - no reboot. Both of these already run together
        // from the SD-mount timer at boot, so this is the same path the launcher uses.
        scanUserApps();
        rebuildAppGrid();
    }
    return ok;
#else
    (void)idx;
    return false;
#endif
}

// Build (or rebuild) the list of app rows on the screen.
void TFTView_320x240::getappsBuildList(void)
{
    if (!getapps_list)
        return;
    lv_obj_clean(getapps_list);

    for (int i = 0; i < storeAppCount; i++) {
        StoreApp &a = storeApps[i];

        lv_obj_t *row = lv_obj_create(getapps_list);
        lv_obj_set_size(row, 286, 62);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c20), LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(0x2a2a30), LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, a.name);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);

        // Size, in the units a person thinks in.
        char meta[32];
        if (a.bytes >= 1024)
            snprintf(meta, sizeof(meta), "%lu KB", (unsigned long)((a.bytes + 512) / 1024));
        else
            snprintf(meta, sizeof(meta), "%lu bytes", (unsigned long)a.bytes);
        lv_obj_t *mt = lv_label_create(row);
        lv_obj_set_style_text_font(mt, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_label_set_text(mt, meta);
        lv_obj_set_style_text_color(mt, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(mt, LV_ALIGN_TOP_LEFT, 0, 20);

        // One line of description, clipped to the row rather than wrapping into the button.
        lv_obj_t *ds = lv_label_create(row);
        lv_obj_set_style_text_font(ds, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_width(ds, 262);
        lv_label_set_long_mode(ds, LV_LABEL_LONG_DOT);
        lv_label_set_text(ds, a.desc);
        lv_obj_set_style_text_color(ds, lv_color_hex(0xdcdce2), LV_PART_MAIN);
        lv_obj_align(ds, LV_ALIGN_TOP_LEFT, 0, 36);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 84, 30);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, 0, -2);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(a.installed ? 0x2c2c2e : 0x30d158), LV_PART_MAIN);
        lv_obj_t *bl = lv_label_create(btn);
        lv_obj_set_style_text_font(bl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_label_set_text(bl, a.installed ? "On device" : "Install");
        lv_obj_set_style_text_color(bl, lv_color_hex(a.installed ? 0x8e8e93 : 0x000000), LV_PART_MAIN);
        lv_obj_center(bl);
        if (!a.installed) {
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(
                btn,
                [](lv_event_t *e) {
                    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
                    int idx = (int)(intptr_t)lv_obj_get_user_data(b);
                    lv_obj_t *lbl = lv_obj_get_child(b, 0);
                    // Downloading blocks this task for a moment; say so before it starts.
                    if (lbl)
                        lv_label_set_text(lbl, "...");
                    lv_refr_now(NULL);
                    bool ok = THIS->getappsInstall(idx);
                    if (lbl)
                        lv_label_set_text(lbl, ok ? "On device" : "Failed");
                    lv_obj_set_style_bg_color(b, lv_color_hex(ok ? 0x2c2c2e : 0xff453a), LV_PART_MAIN);
                    if (ok) {
                        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8e8e93), LV_PART_MAIN);
                        // Left tappable on purpose: installing again just rewrites the same
                        // file, which is also how you'd pull down an updated version.
                        lv_label_set_text(THIS->getapps_status, "Added to your home screen");
                    } else {
                        lv_label_set_text(THIS->getapps_status, "Download failed - try again");
                    }
                },
                LV_EVENT_CLICKED, NULL);
        }
    }
}

// The pump: bring Wi-Fi up if we have to, then fetch the catalog once.
void TFTView_320x240::getappsPump(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (getapps_loaded)
        return;

    if (!tdeck_wifi_connected()) {
        if (lv_tick_get() > getapps_deadline) {
            lv_label_set_text(getapps_status, "Couldn't connect to Wi-Fi");
            if (getapps_timer) {
                lv_timer_delete(getapps_timer);
                getapps_timer = nullptr;
            }
        }
        return;
    }

    lv_label_set_text(getapps_status, "Getting the list...");
    lv_refr_now(NULL);

    int len = 0;
    uint8_t *buf = getappsGet(kGetAppsCatalogUrl, &len);
    getapps_loaded = true; // one attempt per screen visit either way
    if (getapps_timer) {
        lv_timer_delete(getapps_timer);
        getapps_timer = nullptr;
    }
    if (!buf) {
        lv_label_set_text(getapps_status, "Couldn't reach the app list");
        return;
    }
    int n = getappsParseCatalog((const char *)buf, len);
    heap_caps_free(buf);
    if (n <= 0) {
        lv_label_set_text(getapps_status, "No apps available right now");
        return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "%d app%s available", n, n == 1 ? "" : "s");
    lv_label_set_text(getapps_status, msg);
    getappsBuildList();
#endif
}

// Give back everything the screen was holding: the pump timer, the TLS client's ~45KB, and
// Wi-Fi itself if we were the ones who switched it on. Safe to call more than once.
void TFTView_320x240::closeGetApps(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (getapps_timer) {
        lv_timer_delete(getapps_timer);
        getapps_timer = nullptr;
    }
    if (getappsClient) {
        delete getappsClient;
        getappsClient = nullptr;
    }
    if (getapps_own_wifi) { // leave the Settings toggle alone if the user had it on already
        tdeck_wifi_disconnect_now();
        getapps_own_wifi = false;
    }
#endif
}

void TFTView_320x240::openGetApps(void)
{
#ifdef ARDUINO_ARCH_ESP32
    if (!getapps_screen) {
        getapps_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(getapps_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(getapps_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(getapps_screen);
        lv_label_set_text(title, "Get Apps");
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        getapps_status = lv_label_create(getapps_screen);
        lv_obj_set_style_text_font(getapps_status, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(getapps_status, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_align(getapps_status, LV_ALIGN_TOP_MID, 0, 30);
        lv_label_set_text(getapps_status, "");

        // Scrolling list of apps, sitting above the Back button.
        getapps_list = lv_obj_create(getapps_screen);
        lv_obj_set_size(getapps_list, 310, 150);
        lv_obj_align(getapps_list, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_set_style_bg_opa(getapps_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(getapps_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(getapps_list, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(getapps_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(getapps_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(getapps_list, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_t *back = lv_btn_create(getapps_screen);
        lv_obj_set_size(back, 80, 34);
        lv_obj_set_style_bg_color(back, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
        lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, "Back");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_center(bl);
        lv_obj_add_event_cb(
            back,
            [](lv_event_t *) {
                if (THIS->launcher_screen)
                    lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            },
            LV_EVENT_CLICKED, NULL);

        // Safety net, same as the File Share screen: leaving by ANY route - Back, the
        // trackball double-click to Home, the screen locking - tears Wi-Fi back down.
        // Hanging it off the Back button alone left Wi-Fi running (and the battery
        // draining) whenever the user double-clicked out instead.
        lv_obj_add_event_cb(
            getapps_screen, [](lv_event_t *) { THIS->closeGetApps(); }, LV_EVENT_SCREEN_UNLOADED, NULL);
    }

    lv_screen_load_anim(getapps_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

    // Fetch fresh every visit, so a newly published app shows up without a reboot.
    getapps_loaded = false;
    lv_obj_clean(getapps_list);

    bool already = tdeck_wifi_connected();
    getapps_own_wifi = !already;
    if (!already) {
        if (!tdeck_wifi_connect_now(db.config.network.wifi_ssid, db.config.network.wifi_psk)) {
            lv_label_set_text(getapps_status, "Set up Wi-Fi in Settings first");
            return;
        }
        lv_label_set_text(getapps_status, "Turning Wi-Fi on...");
    } else {
        lv_label_set_text(getapps_status, "Getting the list...");
    }
    getapps_deadline = lv_tick_get() + 25000;
    if (!getapps_timer)
        getapps_timer = lv_timer_create([](lv_timer_t *) { THIS->getappsPump(); }, 200, NULL);
#endif
}

// ===== Maps app: user pins (live only on the Maps app's own map) ==========================
namespace {
const uint32_t kPinColors[] = {0xff453a, 0xff9f0a, 0xffd60a, 0x30d158, 0x64d2ff, 0x0a84ff, 0xbf5af2};
const int kPinColorCount = 7;
} // namespace

TFTView_320x240::MapPin *TFTView_320x240::findPin(uint32_t id)
{
    for (auto &p : mapPins)
        if (p.id == id)
            return &p;
    return nullptr;
}

lv_obj_t *TFTView_320x240::makePinMarker(uint32_t color)
{
    lv_obj_t *m = lv_obj_create(maps_map_container);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 14, 14);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(m, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(m, 2, LV_PART_MAIN);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    return m;
}

// The pin's name, shown as a small tag beside its dot. A dark, semi-transparent pill keeps it
// readable over any map tile; a thin border in the pin's own color ties it to the dot.
lv_obj_t *TFTView_320x240::makePinLabel(const char *name, uint32_t color)
{
    lv_obj_t *l = lv_label_create(maps_map_container);
    lv_label_set_text(l, name ? name : "");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(l, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(l, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(l, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(l, 1, LV_PART_MAIN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void TFTView_320x240::drawAllPins(void)
{
    if (!userMap)
        return;
    if (!drawPinCB) {
        drawPinCB = [this](uint32_t id, uint16_t x, uint16_t y, uint8_t zoom) {
            MapPin *p = findPin(id);
            if (!p || !p->marker)
                return;
            if (!x && !y && !zoom) { // hide (off-screen / filtered)
                lv_obj_add_flag(p->marker, LV_OBJ_FLAG_HIDDEN);
                if (p->labelObj)
                    lv_obj_add_flag(p->labelObj, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_clear_flag(p->marker, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(p->marker);
            lv_obj_set_pos(p->marker, x - 7, y - 7); // 14x14 dot centered on the point
            if (p->labelObj) { // name tag sits just to the right of the dot
                lv_obj_clear_flag(p->labelObj, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(p->labelObj);
                lv_obj_set_pos(p->labelObj, x + 10, y - 8);
            }
        };
    }
    for (auto &p : mapPins)
        userMap->add(p.id, p.lat, p.lon, drawPinCB);
}

void TFTView_320x240::dropPinAt(float lat, float lon)
{
    if (!userMap)
        return;
    MapPin p{};
    p.id = nextPinId++;
    p.lat = lat;
    p.lon = lon;
    p.color = kPinColors[p.id % kPinColorCount];
    p.whenEpoch = 0; // TODO: wire device RTC time
    snprintf(p.label, sizeof(p.label), "Pin %u", (unsigned)p.id);
    p.marker = makePinMarker(p.color);
    p.labelObj = makePinLabel(p.label, p.color);
    mapPins.push_back(p);
    userMap->add(p.id, lat, lon, drawPinCB); // draws it immediately
    // Confirm on-screen whether it actually persisted — this is how we're chasing the
    // "pins don't survive reboot" report without digging through files.
    mapsShowNotice(savePins() ? "Pin saved" : "Pin SAVE FAILED (SD?)");
}

void TFTView_320x240::deletePin(uint32_t id)
{
    for (auto it = mapPins.begin(); it != mapPins.end(); ++it) {
        if (it->id == id) {
            if (userMap)
                userMap->remove(id);
            if (it->marker)
                lv_obj_delete(it->marker);
            if (it->labelObj)
                lv_obj_delete(it->labelObj);
            mapPins.erase(it);
            savePins();
            return;
        }
    }
}

void TFTView_320x240::centerOnPin(uint32_t id)
{
    MapPin *p = findPin(id);
    if (p && userMap) {
        userMap->setScrolledPosition(p->lat, p->lon); // center() inside marks the map for redraw
    }
}

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
bool TFTView_320x240::savePins(void)
{
    FsFile f = SDFs.open("/pins.csv", O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return false;
    char line[112];
    for (auto &p : mapPins) {
        snprintf(line, sizeof(line), "%u|%.6f|%.6f|%u|%u|%s\n", (unsigned)p.id, p.lat, p.lon, (unsigned)p.color,
                 (unsigned)p.whenEpoch, p.label);
        f.print(line);
    }
    f.sync(); // force the write + directory entry out to the card before we drop the handle
    f.close();
    // Verify it actually landed: re-open and confirm content is there.
    FsFile v = SDFs.open("/pins.csv", O_RDONLY);
    bool ok = v && (mapPins.empty() || v.size() > 0);
    if (v)
        v.close();
    return ok;
}

void TFTView_320x240::loadPins(void)
{
    if (pinsLoaded)
        return;
    pinsLoaded = true;
    if (!SDFs.exists("/pins.csv")) {
        diagLog("pins load: /pins.csv MISSING (nothing was saved)");
        return;
    }
    FsFile f = SDFs.open("/pins.csv", O_RDONLY);
    if (!f) {
        diagLog("pins load: /pins.csv exists but open FAILED");
        return;
    }
    char line[160];
    while (f.fgets(line, sizeof(line)) > 0) { // SdFat line reader (keeps the trailing newline)
        // Parse "id|lat|lon|color|when|label" WITHOUT scanf %f. THE PINS BUG: on this
        // ESP32's newlib-nano, scanf's float conversion is compiled out, so sscanf("%f")
        // silently failed and EVERY saved pin was dropped on load (the file was fine — the
        // reader was broken). strtof/strtoul are unaffected.
        char *endp = line;
        unsigned id = strtoul(line, &endp, 10);
        if (*endp != '|')
            continue;
        float lat = strtof(endp + 1, &endp);
        if (*endp != '|')
            continue;
        float lon = strtof(endp + 1, &endp);
        if (*endp != '|')
            continue;
        unsigned color = strtoul(endp + 1, &endp, 10);
        if (*endp != '|')
            continue;
        unsigned when = strtoul(endp + 1, &endp, 10);
        if (*endp != '|')
            continue;
        // the label is everything after the 5th '|'
        const char *lbl = line;
        int bars = 0;
        while (*lbl && bars < 5) {
            if (*lbl == '|')
                bars++;
            lbl++;
        }
        MapPin p{};
        p.id = id;
        p.lat = lat;
        p.lon = lon;
        p.color = color ? color : kPinColors[id % kPinColorCount];
        p.whenEpoch = when;
        strncpy(p.label, lbl, sizeof(p.label) - 1);
        for (char *c = p.label; *c; c++)
            if (*c == '\n' || *c == '\r')
                *c = 0;
        p.marker = makePinMarker(p.color);
        p.labelObj = makePinLabel(p.label, p.color);
        mapPins.push_back(p);
        if (id >= nextPinId)
            nextPinId = id + 1;
    }
    f.close();
    char msg[48];
    snprintf(msg, sizeof(msg), "pins load: %u loaded from SD", (unsigned)mapPins.size());
    diagLog(msg);
}

// Append a line to /diaglog.txt (SD). Persistent, survives reboots — our only
// log channel on a board where serial resets the chip.
void TFTView_320x240::diagLog(const char *line)
{
    if (!sdCard)
        return;
    FsFile f = SDFs.open("/diaglog.txt", O_WRONLY | O_CREAT | O_APPEND);
    if (!f)
        return;
    f.print(line);
    f.print("\n");
    f.sync();
    f.close();
}
#else
bool TFTView_320x240::savePins(void) { return false; }
void TFTView_320x240::loadPins(void) { pinsLoaded = true; }
void TFTView_320x240::diagLog(const char *) {}
#endif

void TFTView_320x240::closePinsList(void)
{
    if (pins_overlay) {
        lv_obj_delete_async(pins_overlay); // safe to call from within a child's event callback
        pins_overlay = nullptr;
    }
}

void TFTView_320x240::openPinsList(void)
{
    closePinsList();
    pins_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pins_overlay);
    lv_obj_set_size(pins_overlay, 320, 240);
    lv_obj_set_style_bg_color(pins_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pins_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(pins_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(pins_overlay);
    lv_label_set_text(title, "Pins");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *closeBtn = lv_btn_create(pins_overlay);
    lv_obj_set_size(closeBtn, 62, 28);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t *) { THIS->closePinsList(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);

    lv_obj_t *list = lv_obj_create(pins_overlay);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 314, 194);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (mapPins.empty()) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No pins yet.\nLong-press the map to drop one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        return;
    }

    for (auto &p : mapPins) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 302, 44);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(p.color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, p.label);
        lv_obj_set_style_text_color(name, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_width(name, 112); // bounded so long names don't run under the buttons
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 30, 0);

        // Name (rename), Go (center map), Del — three 44px buttons across the right.
        lv_obj_t *ren = lv_btn_create(row);
        lv_obj_set_size(ren, 44, 30);
        lv_obj_align(ren, LV_ALIGN_RIGHT_MID, -104, 0);
        lv_obj_set_style_bg_color(ren, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
        lv_obj_add_event_cb(
            ren,
            [](lv_event_t *e) {
                uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
                lv_async_call([](void *ud) { THIS->openRenamePin((uint32_t)(uintptr_t)ud); }, (void *)(uintptr_t)id);
            },
            LV_EVENT_CLICKED, (void *)(uintptr_t)p.id);
        lv_obj_t *rl = lv_label_create(ren);
        lv_label_set_text(rl, "Name");
        lv_obj_set_style_text_font(rl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_center(rl);

        lv_obj_t *go = lv_btn_create(row);
        lv_obj_set_size(go, 44, 30);
        lv_obj_align(go, LV_ALIGN_RIGHT_MID, -56, 0);
        lv_obj_add_event_cb(
            go,
            [](lv_event_t *e) {
                uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
                THIS->centerOnPin(id);
                THIS->closePinsList();
            },
            LV_EVENT_CLICKED, (void *)(uintptr_t)p.id);
        lv_obj_t *gl = lv_label_create(go);
        lv_label_set_text(gl, "Go");
        lv_obj_center(gl);

        lv_obj_t *del = lv_btn_create(row);
        lv_obj_set_size(del, 44, 30);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_color(del, lv_color_hex(0x8a2a24), LV_PART_MAIN);
        lv_obj_add_event_cb(
            del,
            [](lv_event_t *e) {
                uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
                THIS->deletePin(id);
                lv_async_call([](void *) { THIS->openPinsList(); }, nullptr); // rebuild after this event
            },
            LV_EVENT_CLICKED, (void *)(uintptr_t)p.id);
        lv_obj_t *dl = lv_label_create(del);
        lv_label_set_text(dl, "Del");
        lv_obj_center(dl);
    }
}

// Full-screen, readable memory/crash readout — the "hard to see the numbers" fix.
// Opened by tapping the top-bar readout. Tap anywhere to close.
void TFTView_320x240::showDiagnostics(void)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, 320, 240);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        ov, [](lv_event_t *e) { lv_obj_delete_async((lv_obj_t *)lv_event_get_target(e)); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Diagnostics");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &ui_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *body = lv_label_create(ov);
    char stall[112] = "";
    if (tdeck_prev_stall_ms()) // the freeze detector's verdict from the previous session
        snprintf(stall, sizeof(stall), "\nLoop stalled in %s for %lus\nspiLock: %s", tdeck_prev_stall_thread(),
                 (unsigned long)(tdeck_prev_stall_ms() / 1000), tdeck_prev_stall_lock());
    char buf[384];
    snprintf(buf, sizeof(buf),
             "Last restart:  %s%s\n\n"
             "Fast RAM   free %luk   low %luk\n"
             "PSRAM      free %luk   low %luk\n\n"
             "Previous run's worst:\n"
             "   fast %luk    psram %luk",
             tdeck_prev_reason_str(), stall, (unsigned long)(tdeck_free_heap() / 1024),
             (unsigned long)(tdeck_min_free_heap() / 1024), (unsigned long)(tdeck_free_psram() / 1024),
             (unsigned long)(tdeck_min_free_psram() / 1024), (unsigned long)(tdeck_prev_heap_low() / 1024),
             (unsigned long)(tdeck_prev_psram_low() / 1024));
    lv_label_set_text(body, buf);
    lv_obj_set_style_text_color(body, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &ui_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 16, 56);

    lv_obj_t *hint = lv_label_create(ov);
    lv_label_set_text(hint, "tap to close");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// Once per boot, when SD is up: append a line to /diaglog.txt if the last restart
// was a genuine fault. A persistent crash history that survives reboots (serial is
// dead on this board), decodable later.
void TFTView_320x240::logDiagBoot(void)
{
    static bool done = false;
    if (done)
        return;
#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
    if (!sdCard) // wait until the card is mounted before writing
        return;
#endif
    done = true;
    if (!tdeck_prev_reason_bad()) // only log real faults, not clean power-ons/restarts
        return;
    char line[200];
    if (tdeck_prev_stall_ms())
        snprintf(line, sizeof(line),
                 "restart=%s  stalled_in=%s  stalled_for=%lus  spilock=[%s]  psram_low=%luk  ram_low=%luk",
                 tdeck_prev_reason_str(), tdeck_prev_stall_thread(), (unsigned long)(tdeck_prev_stall_ms() / 1000),
                 tdeck_prev_stall_lock(), (unsigned long)(tdeck_prev_psram_low() / 1024),
                 (unsigned long)(tdeck_prev_heap_low() / 1024));
    else
        snprintf(line, sizeof(line), "restart=%s  psram_low=%luk  ram_low=%luk", tdeck_prev_reason_str(),
                 (unsigned long)(tdeck_prev_psram_low() / 1024), (unsigned long)(tdeck_prev_heap_low() / 1024));
    diagLog(line);
}

// Apply a new name to a pin and persist it.
void TFTView_320x240::renamePin(uint32_t id, const char *name)
{
    MapPin *p = findPin(id);
    if (!p || !name || !name[0])
        return;
    strncpy(p->label, name, sizeof(p->label) - 1);
    p->label[sizeof(p->label) - 1] = 0;
    for (char *c = p->label; *c; c++)
        if (*c == '|' || *c == '\n' || *c == '\r') // keep the CSV parseable
            *c = ' ';
    if (p->labelObj) // update the name tag shown on the map
        lv_label_set_text(p->labelObj, p->label);
    savePins();
}

void TFTView_320x240::closeRenamePin(void)
{
    endTextEntry(); // stop the focus guard before its textarea is deleted
    if (rename_overlay) {
        lv_obj_delete_async(rename_overlay);
        rename_overlay = nullptr;
        rename_ta = nullptr;
    }
}

// Focus a textarea for the T-Deck's PHYSICAL keyboard and keep it focused. The trackball's
// encoder can wander the LVGL focus, so a light guard timer re-grabs it. This is the same
// proven pattern the WiFi entry + Notes editor use — no on-screen (virtual) keyboard.
void TFTView_320x240::beginTextEntry(lv_obj_t *ta)
{
    text_entry_ta = ta;
    lv_group_t *g = lv_group_get_default();
    if (g) {
        lv_group_add_obj(g, ta);
        lv_group_focus_obj(ta);
    }
    endTextEntry(); // never stack two guards
    text_entry_guard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            if (g && THIS->text_entry_ta && lv_group_get_focused(g) != THIS->text_entry_ta)
                lv_group_focus_obj(THIS->text_entry_ta);
        },
        400, NULL);
}

void TFTView_320x240::endTextEntry(void)
{
    if (text_entry_guard) {
        lv_timer_delete(text_entry_guard);
        text_entry_guard = nullptr;
    }
    text_entry_ta = nullptr;
}

// Rename overlay: a text box pre-filled with the pin's name. Typed on the T-Deck's physical
// keyboard (Enter saves). OK saves, X cancels; either way we return to the pins list.
void TFTView_320x240::openRenamePin(uint32_t id)
{
    MapPin *p = findPin(id);
    if (!p)
        return;
    closePinsList();
    renaming_pin_id = id;

    rename_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(rename_overlay);
    lv_obj_set_size(rename_overlay, 320, 240);
    lv_obj_set_style_bg_color(rename_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rename_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(rename_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(rename_overlay);
    lv_label_set_text(title, "Name this pin");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *hint = lv_label_create(rename_overlay);
    lv_obj_set_width(hint, 296);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(hint, "Type on the keyboard, then Enter or OK");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 32);

    rename_ta = lv_textarea_create(rename_overlay);
    lv_textarea_set_one_line(rename_ta, true);
    lv_textarea_set_max_length(rename_ta, sizeof(p->label) - 1);
    lv_textarea_set_text(rename_ta, p->label);
    lv_obj_set_width(rename_ta, 292);
    lv_obj_align(rename_ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(
        rename_ta,
        [](lv_event_t *) {
            THIS->renamePin(THIS->renaming_pin_id, lv_textarea_get_text(THIS->rename_ta));
            THIS->closeRenamePin();
            lv_async_call([](void *) { THIS->openPinsList(); }, nullptr);
        },
        LV_EVENT_READY, NULL);

    lv_obj_t *okBtn = lv_btn_create(rename_overlay);
    lv_obj_set_size(okBtn, 120, 46);
    lv_obj_align(okBtn, LV_ALIGN_BOTTOM_LEFT, 20, -24);
    lv_obj_t *okl = lv_label_create(okBtn);
    lv_label_set_text(okl, "OK");
    lv_obj_center(okl);
    lv_obj_add_event_cb(
        okBtn,
        [](lv_event_t *) {
            THIS->renamePin(THIS->renaming_pin_id, lv_textarea_get_text(THIS->rename_ta));
            THIS->closeRenamePin();
            lv_async_call([](void *) { THIS->openPinsList(); }, nullptr);
        },
        LV_EVENT_CLICKED, NULL);

    lv_obj_t *cxBtn = lv_btn_create(rename_overlay);
    lv_obj_set_size(cxBtn, 120, 46);
    lv_obj_align(cxBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -24);
    lv_obj_set_style_bg_color(cxBtn, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_t *cxl = lv_label_create(cxBtn);
    lv_label_set_text(cxl, "Cancel");
    lv_obj_center(cxl);
    lv_obj_add_event_cb(
        cxBtn,
        [](lv_event_t *) {
            THIS->closeRenamePin();
            lv_async_call([](void *) { THIS->openPinsList(); }, nullptr);
        },
        LV_EVENT_CLICKED, NULL);

    beginTextEntry(rename_ta);
}

void TFTView_320x240::openFlashlight(void)
{
    if (!flashlight_screen) {
        flashlight_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(flashlight_screen, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_clear_flag(flashlight_screen, LV_OBJ_FLAG_SCROLLABLE);

        // full-size transparent button catches a tap anywhere -> back to the launcher
        lv_obj_t *tap = lv_btn_create(flashlight_screen);
        lv_obj_remove_style_all(tap);
        lv_obj_set_size(tap, LV_PCT(100), LV_PCT(100));
        lv_obj_add_event_cb(
            tap,
            [](lv_event_t *) {
                if (THIS->launcher_screen)
                    lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            },
            LV_EVENT_CLICKED, NULL);

        lv_obj_t *hint = lv_label_create(flashlight_screen);
        lv_label_set_text(hint, "Tap to turn off");
        lv_obj_set_style_text_color(hint, lv_color_hex(0xa0a0a0), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

        // restore brightness + stop the keep-awake whenever we leave (tap OR double-click Home)
        lv_obj_add_event_cb(
            flashlight_screen,
            [](lv_event_t *) {
                THIS->displaydriver->setBrightness((uint8_t)THIS->flashlight_saved_brightness);
                if (THIS->flashlight_keepawake)
                    lv_timer_pause(THIS->flashlight_keepawake);
            },
            LV_EVENT_SCREEN_UNLOADED, NULL);

        flashlight_keepawake = lv_timer_create([](lv_timer_t *) { lv_display_trigger_activity(NULL); }, 4000, NULL);
        lv_timer_pause(flashlight_keepawake);
    }

    flashlight_saved_brightness = db.uiConfig.screen_brightness ? db.uiConfig.screen_brightness : 153;
    displaydriver->setBrightness(255); // full brightness while the light is on
    lv_display_trigger_activity(NULL);
    lv_timer_resume(flashlight_keepawake);
    lv_screen_load_anim(flashlight_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

namespace
{
// "12.3 GB" / "512 KB" / "97 B" into buf
void fmtBytes(uint64_t b, char *buf, size_t cap)
{
    if (b >= 1024ULL * 1024 * 1024)
        snprintf(buf, cap, "%.1f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024ULL * 1024)
        snprintf(buf, cap, "%llu MB", b / (1024 * 1024));
    else if (b >= 1024)
        snprintf(buf, cap, "%llu KB", b / 1024);
    else
        snprintf(buf, cap, "%llu B", b);
}

// join dir + name -> dst ("/" + "foo" -> "/foo"; "/maps" + "0" -> "/maps/0")
void joinPath(char *dst, size_t cap, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(dst, cap, "/%s", name);
    else
        snprintf(dst, cap, "%s/%s", dir, name);
}

#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
// One read/write handle that can sit on either volume, so the copy loop below
// is written once instead of per SD/flash combination.
struct VolFile {
    FsFile sd;
    fs::File fl;
    bool isSd = false;

    bool openRead(bool onSd, const char *path)
    {
        isSd = onSd;
        if (isSd) {
            sd = SDFs.open(path, O_RDONLY);
            return (bool)sd;
        }
        fl = fileSystem.open(path, "r");
        return (bool)fl && !fl.isDirectory();
    }
    bool openWrite(bool onSd, const char *path)
    {
        isSd = onSd;
        if (isSd) {
            sd = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
            return (bool)sd;
        }
        fl = fileSystem.open(path, "w");
        return (bool)fl;
    }
    int read(uint8_t *buf, size_t n) { return isSd ? sd.read(buf, n) : (int)fl.read(buf, n); }
    int write(const uint8_t *buf, size_t n) { return isSd ? (int)sd.write(buf, n) : (int)fl.write(buf, n); }
    uint64_t size() { return isSd ? sd.size() : fl.size(); }
    void close()
    {
        if (isSd)
            sd.close();
        else
            fl.close();
    }
};

bool volExists(bool onSd, const char *path)
{
    return onSd ? SDFs.exists(path) : fileSystem.exists(path);
}
void volRemove(bool onSd, const char *path)
{
    if (onSd)
        SDFs.remove(path);
    else
        fileSystem.remove(path);
}
bool volMkdir(bool onSd, const char *path)
{
    return onSd ? SDFs.mkdir(path) : fileSystem.mkdir(path);
}
bool volRename(bool onSd, const char *from, const char *to)
{
    return onSd ? SDFs.rename(from, to) : fileSystem.rename(from, to);
}

// Delete a folder and everything inside it. We snapshot each directory's entries
// before touching them (so we never mutate a directory while its iterator is open),
// recurse into sub-folders, then remove this folder itself. Depth-capped and guarded
// against ever operating on the volume root, so a stray call can't wipe the card.
bool volRemoveTree(bool onSd, const char *path, int depth)
{
    if (depth > 6)                                 // real folders here are 1-2 deep; cap runaway
        return false;
    if (!path || !path[0] || strcmp(path, "/") == 0)
        return false;

    static constexpr int kMax = 40;
    char names[kMax][40];
    bool dirs[kMax];
    int n = 0;

    if (onSd) {
        FsFile dir = SDFs.open(path, O_RDONLY);
        if (dir && dir.isDirectory()) {
            FsFile e;
            while ((e = dir.openNextFile()) && n < kMax) {
                e.getName(names[n], sizeof(names[0]));
                dirs[n] = e.isDirectory();
                e.close();
                if (names[n][0])
                    n++;
            }
            if (e)
                e.close();
            dir.close();
        }
    } else {
        fs::File dir = fileSystem.open(path);
        if (dir && dir.isDirectory()) {
            fs::File e;
            while ((e = dir.openNextFile()) && n < kMax) {
                const char *full = e.name();
                const char *nm = strrchr(full, '/');
                nm = nm ? nm + 1 : full;
                snprintf(names[n], sizeof(names[0]), "%s", nm);
                dirs[n] = e.isDirectory();
                e.close();
                if (names[n][0])
                    n++;
            }
            if (e)
                e.close();
            dir.close();
        }
    }

    for (int i = 0; i < n; i++) {
        char child[256];
        if (strcmp(path, "/") == 0)
            snprintf(child, sizeof(child), "/%s", names[i]);
        else
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (dirs[i])
            volRemoveTree(onSd, child, depth + 1);
        else
            volRemove(onSd, child);
    }
    return onSd ? SDFs.rmdir(path) : fileSystem.rmdir(path);
}

// Each trashed file gets a tiny "<name>.where" note beside it holding its original
// full path, so Restore can put it back exactly where it came from.
void trashNotePath(char *dst, size_t cap, const char *trashName)
{
    snprintf(dst, cap, "/.trash/%s.where", trashName);
}
void writeTrashNote(bool onSd, const char *trashName, const char *origPath)
{
    char notePath[280];
    trashNotePath(notePath, sizeof(notePath), trashName);
    VolFile w;
    if (w.openWrite(onSd, notePath)) {
        w.write((const uint8_t *)origPath, strlen(origPath));
        w.close();
    }
}
void readTrashNote(bool onSd, const char *trashName, char *out, size_t cap)
{
    out[0] = 0;
    char notePath[280];
    trashNotePath(notePath, sizeof(notePath), trashName);
    VolFile r;
    if (r.openRead(onSd, notePath)) {
        int n = r.read((uint8_t *)out, cap - 1);
        r.close();
        if (n < 0)
            n = 0;
        out[n] = 0;
        char *nl = strchr(out, '\n');
        if (nl)
            *nl = 0;
    }
}
#endif
} // namespace

// Free space, measured at most ONCE per volume and then cached. SdFat's freeBytes()
// walks the card's entire allocation table — several seconds on a large card — and it
// was being called on every navigation, which made the whole browser feel broken.
// The cache is invalidated only by operations that change usage (paste, delete forever).
uint64_t TFTView_320x240::filesFreeBytes(bool onSd)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    int i = onSd ? 0 : 1;
    if (!filesFreeKnown[i]) {
        filesFreeCache[i] = onSd ? (sdCard ? sdCard->freeBytes() : 0)
                                 : (uint64_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
        filesFreeKnown[i] = true;
    }
    return filesFreeCache[i];
#else
    return 0;
#endif
}

void TFTView_320x240::openFiles(void)
{
    if (!files_screen) {
        files_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(files_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(files_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(files_screen);
        lv_label_set_text(title, "Files");
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        files_path_label = lv_label_create(files_screen);
        lv_obj_set_style_text_color(files_path_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_set_width(files_path_label, 300);
        lv_label_set_long_mode(files_path_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(files_path_label, LV_ALIGN_TOP_LEFT, 10, 28);

        files_list = lv_list_create(files_screen);
        lv_obj_set_size(files_list, 320, 152);
        lv_obj_align(files_list, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_set_style_bg_color(files_list, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
        lv_obj_set_style_border_width(files_list, 0, LV_PART_MAIN);

        // bottom bar: free space (left) · Back (center) · Copy/Paste + X (right)
        files_free_label = lv_label_create(files_screen);
        lv_label_set_text(files_free_label, "");
        lv_obj_set_style_text_color(files_free_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(files_free_label, LV_ALIGN_BOTTOM_LEFT, 10, -12);

        // Back: up one folder -> volume root -> storage picker -> launcher
        lv_obj_t *backBtn = lv_btn_create(files_screen);
        lv_obj_set_size(backBtn, 70, 30);
        lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, -14, -4);
        lv_obj_set_style_radius(backBtn, 10, LV_PART_MAIN);
        lv_obj_add_event_cb(
            backBtn,
            [](lv_event_t *e) {
                if (THIS->filesVol == VOL_ROOT) { // storage picker -> exit to Home
                    if (THIS->launcher_screen)
                        lv_screen_load_anim(THIS->launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
                    return;
                }
                if (strcmp(THIS->filesPath, "/") == 0) { // volume root -> storage picker
                    THIS->filesVol = VOL_ROOT;
                    THIS->filesRefresh();
                    return;
                }
                char *slash = strrchr(THIS->filesPath, '/'); // go up one level
                if (slash && slash != THIS->filesPath)
                    *slash = 0;
                else
                    strcpy(THIS->filesPath, "/");
                THIS->filesRefresh();
            },
            LV_EVENT_CLICKED, NULL);
        lv_obj_t *backLbl = lv_label_create(backBtn);
        lv_label_set_text(backLbl, "Back");
        lv_obj_center(backLbl);

        // Copy (a file is selected) / Paste here (a copy is armed)
        files_copy_btn = lv_btn_create(files_screen);
        lv_obj_set_size(files_copy_btn, 100, 30);
        lv_obj_align(files_copy_btn, LV_ALIGN_BOTTOM_RIGHT, -34, -4);
        lv_obj_set_style_radius(files_copy_btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(files_copy_btn, lv_color_hex(0x0a84ff), LV_PART_MAIN);
        lv_obj_add_event_cb(
            files_copy_btn,
            [](lv_event_t *e) {
                if (THIS->filesVol == VOL_TRASH) { // in the trash this button is "Restore"
                    THIS->filesRestoreSelected();
                    return;
                }
                if (!THIS->filesCopyArmed) { // arm: remember the selected file as source
                    if (!THIS->filesSelName[0])
                        return;
                    joinPath(THIS->copySrcPath, sizeof(THIS->copySrcPath), THIS->filesPath, THIS->filesSelName);
                    THIS->copySrcVol = THIS->filesVol;
                    THIS->filesCopyArmed = true;
                    THIS->filesUpdateActionBar();
                } else {
                    THIS->filesCopyNow(); // paste into the current directory
                }
            },
            LV_EVENT_CLICKED, NULL);
        files_copy_lbl = lv_label_create(files_copy_btn);
        lv_label_set_text(files_copy_lbl, "Copy");
        lv_obj_center(files_copy_lbl);

        // Trash (browsing: move file to /.trash, instantly undoable) /
        // Delete (in the trash view: permanent, needs a second "Sure?" tap)
        files_del_btn = lv_btn_create(files_screen);
        lv_obj_set_size(files_del_btn, 64, 30);
        lv_obj_align(files_del_btn, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
        lv_obj_set_style_radius(files_del_btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(files_del_btn, lv_color_hex(0xff453a), LV_PART_MAIN);
        lv_obj_add_event_cb(
            files_del_btn,
            [](lv_event_t *e) {
                if (!THIS->filesSelName[0])
                    return;
                if (THIS->filesVol != VOL_TRASH) { // recoverable — no confirmation needed
                    THIS->filesTrashSelected();
                    return;
                }
                if (!THIS->filesDelConfirm) { // permanent — ask once
                    THIS->filesDelConfirm = true;
                    lv_label_set_text(THIS->files_del_lbl, "Sure?");
                    lv_obj_set_style_bg_color(THIS->files_del_btn, lv_color_hex(0x8b1a12), LV_PART_MAIN);
                    return;
                }
                THIS->filesDeleteForever();
            },
            LV_EVENT_CLICKED, NULL);
        files_del_lbl = lv_label_create(files_del_btn);
        lv_label_set_text(files_del_lbl, "Trash");
        lv_obj_center(files_del_lbl);

        // X — cancel a pending copy
        files_cancel_btn = lv_btn_create(files_screen);
        lv_obj_set_size(files_cancel_btn, 30, 30);
        lv_obj_align(files_cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
        lv_obj_set_style_radius(files_cancel_btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(files_cancel_btn, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
        lv_obj_add_event_cb(
            files_cancel_btn,
            [](lv_event_t *e) {
                THIS->filesCopyArmed = false;
                THIS->copySrcPath[0] = 0;
                THIS->filesUpdateActionBar();
            },
            LV_EVENT_CLICKED, NULL);
        lv_obj_t *xLbl = lv_label_create(files_cancel_btn);
        lv_label_set_text(xLbl, "X");
        lv_obj_center(xLbl);

        // + Folder — make a new folder here (only shown when nothing is selected)
        files_newfolder_btn = lv_btn_create(files_screen);
        lv_obj_set_size(files_newfolder_btn, 100, 30);
        lv_obj_align(files_newfolder_btn, LV_ALIGN_BOTTOM_RIGHT, -2, -4);
        lv_obj_set_style_radius(files_newfolder_btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(files_newfolder_btn, lv_color_hex(0x48484a), LV_PART_MAIN);
        lv_obj_add_event_cb(
            files_newfolder_btn, [](lv_event_t *) { THIS->filesNewFolderPrompt(); }, LV_EVENT_CLICKED, NULL);
        files_newfolder_lbl = lv_label_create(files_newfolder_btn);
        lv_label_set_text(files_newfolder_lbl, LV_SYMBOL_PLUS " Folder");
        lv_obj_set_style_text_font(files_newfolder_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(files_newfolder_lbl);
    }
    filesRefresh();
    lv_screen_load_anim(files_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Show/hide the bottom-right action buttons to match the current state.
void TFTView_320x240::filesUpdateActionBar(void)
{
    // any re-render resets a pending "Sure?" back to a plain Delete
    filesDelConfirm = false;
    lv_label_set_text(files_del_lbl, filesVol == VOL_TRASH ? "Delete" : "Trash");
    lv_obj_set_style_bg_color(files_del_btn, lv_color_hex(0xff453a), LV_PART_MAIN);
    lv_obj_add_flag(files_newfolder_btn, LV_OBJ_FLAG_HIDDEN); // shown only in the idle branch below

    if (filesVol == VOL_TRASH) { // Restore + permanent Delete for the selected item
        lv_obj_add_flag(files_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        if (filesSelName[0]) {
            lv_obj_clear_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(files_copy_btn, 84, 30);
            lv_obj_align(files_copy_btn, LV_ALIGN_BOTTOM_RIGHT, -70, -4);
            lv_label_set_text(files_copy_lbl, "Restore");
            lv_obj_set_style_bg_color(files_copy_btn, lv_color_hex(0x30d158), LV_PART_MAIN);
            lv_obj_clear_flag(files_del_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(files_del_btn, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (filesCopyArmed) {
        // pasting only makes sense inside a folder, not on the storage picker
        if (filesVol == VOL_ROOT)
            lv_obj_add_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(files_copy_btn, 100, 30);
        lv_obj_align(files_copy_btn, LV_ALIGN_BOTTOM_RIGHT, -36, -4);
        lv_label_set_text(files_copy_lbl, "Paste here");
        lv_obj_set_style_bg_color(files_copy_btn, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_clear_flag(files_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(files_del_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (filesSelName[0]) { // a file or folder is selected
        if (filesSelIsDir) {
            // no single-file Copy for a folder — just offer Trash
            lv_obj_add_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(files_copy_btn, 64, 30);
            lv_obj_align(files_copy_btn, LV_ALIGN_BOTTOM_RIGHT, -70, -4);
            lv_label_set_text(files_copy_lbl, "Copy");
            lv_obj_set_style_bg_color(files_copy_btn, lv_color_hex(0x0a84ff), LV_PART_MAIN);
        }
        lv_obj_add_flag(files_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(files_del_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(files_copy_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(files_cancel_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(files_del_btn, LV_OBJ_FLAG_HIDDEN);
        // idle inside a real volume — offer to make a new folder here
        if (filesVol == VOL_SD || filesVol == VOL_FLASH)
            lv_obj_clear_flag(files_newfolder_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// Paste: copy the armed source file into filesPath on the current volume.
void TFTView_320x240::filesCopyNow(void)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    static constexpr uint64_t kMaxCopyBytes = 8ULL * 1024 * 1024; // keep the UI responsive
    static uint8_t copyBuf[4096];

    const char *base = strrchr(copySrcPath, '/');
    base = base ? base + 1 : copySrcPath;
    char dst[256];
    joinPath(dst, sizeof(dst), filesPath, base);

    bool srcSd = (copySrcVol == VOL_SD);
    bool dstSd = (filesVol == VOL_SD);

    if (srcSd == dstSd && strcmp(dst, copySrcPath) == 0) {
        messageAlert(_("Source and destination are the same"), true);
        return;
    }
    if (volExists(dstSd, dst)) {
        messageAlert(_("A file with this name is already here"), true);
        return; // stays armed — paste somewhere else or cancel
    }

    VolFile src;
    if (!src.openRead(srcSd, copySrcPath)) {
        messageAlert(_("Can't open the source file"), true);
        return;
    }
    uint64_t total = src.size();
    if (total > kMaxCopyBytes) {
        src.close();
        messageAlert(_("File too big to copy on-device"), true);
        return;
    }
    // destination must have room (flash especially — it holds device data)
    uint64_t freeBytes = filesFreeBytes(dstSd);
    if (total + 4096 > freeBytes) {
        src.close();
        messageAlert(_("Not enough space here"), true);
        return;
    }

    VolFile out;
    if (!out.openWrite(dstSd, dst)) {
        src.close();
        messageAlert(_("Can't create the file here"), true);
        return;
    }

    bool ok = true;
    int n;
    while ((n = src.read(copyBuf, sizeof(copyBuf))) > 0) {
        if (out.write(copyBuf, n) != n) {
            ok = false;
            break;
        }
    }
    src.close();
    out.close();

    if (!ok) {
        volRemove(dstSd, dst); // don't leave a half-written file behind
        messageAlert(_("Copy failed"), true);
        return;
    }
    filesCopyArmed = false;
    copySrcPath[0] = 0;
    filesFreeKnown[dstSd ? 0 : 1] = false; // usage changed — re-measure lazily
    filesRefresh();                        // the new file appears in the listing
#else
    messageAlert(_("No storage support in this build"), true);
#endif
}

// Move the selected file into its volume's /.trash (a rename — instant, no data copied),
// leaving a note with its original path so Restore can undo it.
void TFTView_320x240::filesTrashSelected(void)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    bool onSd = (filesVol == VOL_SD);
    char src[256];
    joinPath(src, sizeof(src), filesPath, filesSelName);

    if (!volExists(onSd, "/.trash") && !volMkdir(onSd, "/.trash")) {
        messageAlert(_("Couldn't create the trash folder"), true);
        return;
    }

    // pick a free name inside the trash (same name may have been trashed before)
    char trashName[80], dst[280];
    snprintf(trashName, sizeof(trashName), "%s", filesSelName);
    snprintf(dst, sizeof(dst), "/.trash/%s", trashName);
    for (int i = 2; volExists(onSd, dst) && i < 100; i++) {
        snprintf(trashName, sizeof(trashName), "%s~%d", filesSelName, i);
        snprintf(dst, sizeof(dst), "/.trash/%s", trashName);
    }
    if (volExists(onSd, dst)) {
        messageAlert(_("Trash already has too many copies of this"), true);
        return;
    }

    if (!volRename(onSd, src, dst)) {
        messageAlert(_("Couldn't move it to the trash"), true);
        return;
    }
    writeTrashNote(onSd, trashName, src);

    // if this file was armed as a copy source, the source is gone — disarm
    if (filesCopyArmed && copySrcVol == filesVol && strcmp(copySrcPath, src) == 0) {
        filesCopyArmed = false;
        copySrcPath[0] = 0;
    }
    filesRefresh();
#endif
}

// Trash view: move the selected item back to the exact spot it was trashed from.
void TFTView_320x240::filesRestoreSelected(void)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    if (!filesSelName[1])
        return;
    bool onSd = (filesSelName[0] == 'S'); // trash rows are stored marker-first
    const char *name = &filesSelName[1];

    char src[280], dest[256];
    snprintf(src, sizeof(src), "/.trash/%s", name);
    readTrashNote(onSd, name, dest, sizeof(dest));
    if (!dest[0]) // note missing — restore to the volume root, minus any ~N suffix
        snprintf(dest, sizeof(dest), "/%s", name);

    if (volExists(onSd, dest)) {
        messageAlert(_("Its old spot already has a file with this name"), true);
        return;
    }
    if (!volRename(onSd, src, dest)) {
        messageAlert(_("Couldn't restore it (folder gone?)"), true);
        return;
    }
    char notePath[280];
    trashNotePath(notePath, sizeof(notePath), name);
    volRemove(onSd, notePath);
    filesRefresh();
#endif
}

// Trash view: permanently delete (reached only via the two-tap "Sure?" flow).
void TFTView_320x240::filesDeleteForever(void)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    if (!filesSelName[1])
        return;
    bool onSd = (filesSelName[0] == 'S');
    const char *name = &filesSelName[1];

    char path[280];
    snprintf(path, sizeof(path), "/.trash/%s", name);
    if (filesSelIsDir)
        volRemoveTree(onSd, path, 0); // a folder: wipe it and everything inside
    else
        volRemove(onSd, path);
    trashNotePath(path, sizeof(path), name);
    volRemove(onSd, path);
    filesFreeKnown[onSd ? 0 : 1] = false; // usage changed — re-measure lazily
    filesRefresh();
#endif
}

// Make a new folder named `name` inside the directory currently being browsed.
void TFTView_320x240::filesNewFolderCreate(const char *name)
{
#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    if (!name || (filesVol != VOL_SD && filesVol != VOL_FLASH))
        return;

    // keep it to a single, clean folder name — no path separators or line breaks
    char clean[40];
    int j = 0;
    for (const char *c = name; *c && j < (int)sizeof(clean) - 1; c++)
        if (*c != '/' && *c != '\\' && *c != '\n' && *c != '\r')
            clean[j++] = *c;
    clean[j] = 0;
    while (j > 0 && clean[j - 1] == ' ') // trim trailing spaces
        clean[--j] = 0;
    if (!clean[0])
        return;

    bool onSd = (filesVol == VOL_SD);
    char full[256];
    joinPath(full, sizeof(full), filesPath, clean);
    if (volExists(onSd, full)) {
        messageAlert(_("Something with that name is already here"), true);
        return;
    }
    if (!volMkdir(onSd, full)) {
        messageAlert(_("Couldn't create the folder"), true);
        return;
    }
    filesRefresh(); // the new folder appears in the listing
#endif
}

void TFTView_320x240::closeNewFolder(void)
{
    endTextEntry(); // stop the focus guard before its textarea is deleted
    if (files_newfolder_ovl) {
        lv_obj_delete_async(files_newfolder_ovl);
        files_newfolder_ovl = nullptr;
        files_newfolder_ta = nullptr;
    }
}

// Folder-name overlay: a text box typed on the T-Deck's PHYSICAL keyboard (Enter creates it).
// OK creates the folder; Cancel dismisses. Same physical-keyboard pattern as the pin rename.
void TFTView_320x240::filesNewFolderPrompt(void)
{
    if (filesVol != VOL_SD && filesVol != VOL_FLASH)
        return;

    files_newfolder_ovl = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(files_newfolder_ovl);
    lv_obj_set_size(files_newfolder_ovl, 320, 240);
    lv_obj_set_style_bg_color(files_newfolder_ovl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(files_newfolder_ovl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(files_newfolder_ovl, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(files_newfolder_ovl);
    lv_label_set_text(title, "New folder name");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t *hint = lv_label_create(files_newfolder_ovl);
    lv_obj_set_width(hint, 296);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(hint, "Type on the keyboard, then Enter or OK");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 32);

    files_newfolder_ta = lv_textarea_create(files_newfolder_ovl);
    lv_textarea_set_one_line(files_newfolder_ta, true);
    lv_textarea_set_max_length(files_newfolder_ta, 38);
    lv_obj_set_width(files_newfolder_ta, 292);
    lv_obj_align(files_newfolder_ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(
        files_newfolder_ta,
        [](lv_event_t *) {
            char nm[40];
            snprintf(nm, sizeof(nm), "%s", lv_textarea_get_text(THIS->files_newfolder_ta));
            THIS->closeNewFolder();
            THIS->filesNewFolderCreate(nm);
        },
        LV_EVENT_READY, NULL);

    lv_obj_t *okBtn = lv_btn_create(files_newfolder_ovl);
    lv_obj_set_size(okBtn, 120, 46);
    lv_obj_align(okBtn, LV_ALIGN_BOTTOM_LEFT, 20, -24);
    lv_obj_t *okl = lv_label_create(okBtn);
    lv_label_set_text(okl, "OK");
    lv_obj_center(okl);
    lv_obj_add_event_cb(
        okBtn,
        [](lv_event_t *) {
            char nm[40];
            snprintf(nm, sizeof(nm), "%s", lv_textarea_get_text(THIS->files_newfolder_ta));
            THIS->closeNewFolder();
            THIS->filesNewFolderCreate(nm);
        },
        LV_EVENT_CLICKED, NULL);

    lv_obj_t *cxBtn = lv_btn_create(files_newfolder_ovl);
    lv_obj_set_size(cxBtn, 120, 46);
    lv_obj_align(cxBtn, LV_ALIGN_BOTTOM_RIGHT, -20, -24);
    lv_obj_set_style_bg_color(cxBtn, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_t *cxl = lv_label_create(cxBtn);
    lv_label_set_text(cxl, "Cancel");
    lv_obj_center(cxl);
    lv_obj_add_event_cb(
        cxBtn, [](lv_event_t *) { THIS->closeNewFolder(); }, LV_EVENT_CLICKED, NULL);

    beginTextEntry(files_newfolder_ta);
}

namespace
{
// style a list row for the dark theme + make its icon glyph actually render
// (the app's default font lacks the symbol glyphs; the built-in one has them)
void filesStyleRow(lv_obj_t *btn, uint32_t iconColor)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    if (icon) {
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex(iconColor), LV_PART_MAIN);
    }
}
} // namespace

void TFTView_320x240::filesRefresh(void)
{
    lv_obj_clean(files_list);
    files_sel_row = nullptr;
    filesSelName[0] = 0; // navigation clears the selection (an armed copy survives)
    filesSelIsDir = false;
    filesUpdateActionBar();

#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)
    char freeBuf[24], line[96];

    // ---------- storage picker ----------
    if (filesVol == VOL_ROOT) {
        lv_label_set_text(files_path_label, "Storage");
        lv_label_set_text(files_free_label, "");

        auto pick_cb = [](lv_event_t *e) {
            THIS->filesVol = (FilesVol)(intptr_t)lv_event_get_user_data(e);
            strcpy(THIS->filesPath, "/");
            THIS->filesRefresh();
        };

        if (sdCard) {
            fmtBytes(filesFreeBytes(true), freeBuf, sizeof(freeBuf));
            snprintf(line, sizeof(line), "SD card  —  %s free", freeBuf);
            lv_obj_t *btn = lv_list_add_button(files_list, LV_SYMBOL_SD_CARD, line);
            filesStyleRow(btn, 0x0a84ff);
            lv_obj_add_event_cb(btn, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)VOL_SD);
        } else {
            lv_list_add_text(files_list, "No SD card found");
        }

        fmtBytes(filesFreeBytes(false), freeBuf, sizeof(freeBuf));
        snprintf(line, sizeof(line), "T-Deck memory  —  %s free", freeBuf);
        lv_obj_t *btn = lv_list_add_button(files_list, LV_SYMBOL_SAVE, line);
        filesStyleRow(btn, 0x30d158);
        lv_obj_add_event_cb(btn, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)VOL_FLASH);

        lv_obj_t *tbtn = lv_list_add_button(files_list, LV_SYMBOL_TRASH, "Trash");
        filesStyleRow(tbtn, 0xff453a);
        lv_obj_add_event_cb(tbtn, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)VOL_TRASH);
        return;
    }

    // ---------- trash view (both volumes' /.trash merged) ----------
    if (filesVol == VOL_TRASH) {
        lv_label_set_text(files_path_label, "Trash");
        lv_label_set_text(files_free_label, "");

        auto trash_cb = [](lv_event_t *e) { // tap: select for Restore / Delete
            intptr_t idx = (intptr_t)lv_event_get_user_data(e);
            lv_obj_t *row = (lv_obj_t *)lv_event_get_current_target(e);
            if (THIS->files_sel_row)
                lv_obj_set_style_bg_color(THIS->files_sel_row, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x2c2c54), LV_PART_MAIN);
            THIS->files_sel_row = row;
            snprintf(THIS->filesSelName, sizeof(THIS->filesSelName), "%s", THIS->filesNames[idx]);
            THIS->filesSelIsDir = THIS->filesNameIsDir[idx];
            THIS->filesUpdateActionBar();
        };

        int count = 0;
        for (int pass = 0; pass < 2 && count < kFilesMaxEntries; pass++) {
            bool onSd = (pass == 0);
            if (onSd && !sdCard)
                continue;
            char freeB2[24];
            if (onSd) {
                FsFile dir = SDFs.open("/.trash", O_RDONLY);
                if (!dir || !dir.isDirectory())
                    continue;
                FsFile entry;
                while ((entry = dir.openNextFile()) && count < kFilesMaxEntries) {
                    char name[64];
                    entry.getName(name, sizeof(name));
                    size_t len = strlen(name);
                    bool isdir = entry.isDirectory();
                    if (!isdir && len > 6 && strcmp(name + len - 6, ".where") == 0) {
                        entry.close();
                        continue; // skip the origin notes themselves
                    }
                    filesNameIsDir[count] = isdir;
                    snprintf(filesNames[count], sizeof(filesNames[0]), "S%s", name);
                    if (isdir) {
                        snprintf(line, sizeof(line), "%s  (folder)  · SD", name);
                    } else {
                        fmtBytes(entry.size(), freeB2, sizeof(freeB2));
                        snprintf(line, sizeof(line), "%s  (%s)  · SD", name, freeB2);
                    }
                    lv_obj_t *btn = lv_list_add_button(files_list, isdir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, line);
                    filesStyleRow(btn, isdir ? 0xffd60a : 0x0a84ff);
                    lv_obj_add_event_cb(btn, trash_cb, LV_EVENT_CLICKED, (void *)(intptr_t)count);
                    count++;
                    entry.close();
                }
                if (entry)
                    entry.close();
                dir.close();
            } else {
                fs::File dir = fileSystem.open("/.trash");
                if (!dir || !dir.isDirectory())
                    continue;
                fs::File entry;
                while ((entry = dir.openNextFile()) && count < kFilesMaxEntries) {
                    const char *full = entry.name();
                    const char *name = strrchr(full, '/');
                    name = name ? name + 1 : full;
                    size_t len = strlen(name);
                    bool isdir = entry.isDirectory();
                    if (!isdir && len > 6 && strcmp(name + len - 6, ".where") == 0) {
                        entry.close();
                        continue;
                    }
                    filesNameIsDir[count] = isdir;
                    snprintf(filesNames[count], sizeof(filesNames[0]), "F%s", name);
                    if (isdir) {
                        snprintf(line, sizeof(line), "%s  (folder)  · T-Deck", name);
                    } else {
                        fmtBytes(entry.size(), freeB2, sizeof(freeB2));
                        snprintf(line, sizeof(line), "%s  (%s)  · T-Deck", name, freeB2);
                    }
                    lv_obj_t *btn = lv_list_add_button(files_list, isdir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, line);
                    filesStyleRow(btn, isdir ? 0xffd60a : 0x30d158);
                    lv_obj_add_event_cb(btn, trash_cb, LV_EVENT_CLICKED, (void *)(intptr_t)count);
                    count++;
                    entry.close();
                }
                if (entry)
                    entry.close();
                dir.close();
            }
        }
        if (count == 0)
            lv_list_add_text(files_list, "(trash is empty)");
        return;
    }

    // ---------- browsing a volume ----------
    bool onSd = (filesVol == VOL_SD);
    snprintf(line, sizeof(line), "%s%s", onSd ? "SD:" : "T-Deck:", filesPath);
    lv_label_set_text(files_path_label, line);

    fmtBytes(filesFreeBytes(onSd), freeBuf, sizeof(freeBuf));
    snprintf(line, sizeof(line), "%s free", freeBuf);
    lv_label_set_text(files_free_label, line);

    // entry tap: folders descend; a file becomes the highlighted selection.
    // (a long-press selected the row instead — swallow the release-click that follows)
    auto entry_cb = [](lv_event_t *e) {
        if (THIS->filesSuppressClick) {
            THIS->filesSuppressClick = false;
            return;
        }
        intptr_t idx = (intptr_t)lv_event_get_user_data(e);
        const char *name = THIS->filesNames[idx];
        if (name[0] == '/') { // '/' prefix marks a folder
            if (strlen(THIS->filesPath) + strlen(name) >= sizeof(THIS->filesPath) - 1)
                return; // path would overflow — stay put
            if (strcmp(THIS->filesPath, "/") != 0)
                strcat(THIS->filesPath, name);
            else
                strcpy(THIS->filesPath, name);
            THIS->filesRefresh();
            return;
        }
        // tapping the already-selected .txt again opens it in the Notes editor
        lv_obj_t *row = (lv_obj_t *)lv_event_get_current_target(e);
        size_t nlen = strlen(name);
        if (row == THIS->files_sel_row && THIS->filesVol == VOL_SD && nlen > 4 &&
            (strcmp(name + nlen - 4, ".txt") == 0 || strcmp(name + nlen - 4, ".TXT") == 0)) {
            char full[256];
            joinPath(full, sizeof(full), THIS->filesPath, name);
            notes_open_file(full);
            return;
        }
        // select the file (tap again elsewhere to move the highlight)
        if (THIS->files_sel_row)
            lv_obj_set_style_bg_color(THIS->files_sel_row, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2c2c54), LV_PART_MAIN);
        THIS->files_sel_row = row;
        snprintf(THIS->filesSelName, sizeof(THIS->filesSelName), "%s", name);
        THIS->filesSelIsDir = false;
        THIS->filesUpdateActionBar();
    };

    // long-press any row to select it (needed for folders — a plain tap just opens them).
    // Files can be long-pressed too; it selects them just like a tap.
    auto entry_long_cb = [](lv_event_t *e) {
        intptr_t idx = (intptr_t)lv_event_get_user_data(e);
        const char *name = THIS->filesNames[idx];
        const char *plain = (name[0] == '/') ? name + 1 : name; // strip the folder marker
        lv_obj_t *row = (lv_obj_t *)lv_event_get_current_target(e);
        if (THIS->files_sel_row)
            lv_obj_set_style_bg_color(THIS->files_sel_row, lv_color_hex(0x0d0d0f), LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2c2c54), LV_PART_MAIN);
        THIS->files_sel_row = row;
        snprintf(THIS->filesSelName, sizeof(THIS->filesSelName), "%s", plain);
        THIS->filesSelIsDir = THIS->filesNameIsDir[idx];
        THIS->filesSuppressClick = true; // don't let the release-click descend/re-select
        THIS->filesUpdateActionBar();
    };

    int count = 0;
    bool truncated = false;

    if (onSd) {
        if (!sdCard) {
            lv_list_add_text(files_list, "No SD card found");
            return;
        }
        FsFile dir = SDFs.open(filesPath, O_RDONLY);
        if (!dir || !dir.isDirectory()) {
            lv_list_add_text(files_list, "Can't open this folder");
            return;
        }
        FsFile entry;
        while ((entry = dir.openNextFile()) && count < kFilesMaxEntries) {
            char name[48];
            entry.getName(name, sizeof(name));
            if (name[0] == '.') { // skip hidden/system entries
                entry.close();
                continue;
            }
            lv_obj_t *btn;
            filesNameIsDir[count] = entry.isDirectory();
            if (entry.isDirectory()) {
                // '/' prefix doubles as the folder marker and the path segment
                snprintf(filesNames[count], sizeof(filesNames[0]), "/%s", name);
                btn = lv_list_add_button(files_list, LV_SYMBOL_DIRECTORY, name);
                filesStyleRow(btn, 0xffd60a);
            } else {
                snprintf(filesNames[count], sizeof(filesNames[0]), "%s", name);
                fmtBytes(entry.size(), freeBuf, sizeof(freeBuf));
                snprintf(line, sizeof(line), "%s  (%s)", name, freeBuf);
                btn = lv_list_add_button(files_list, LV_SYMBOL_FILE, line);
                filesStyleRow(btn, 0x8e8e93);
            }
            lv_obj_add_event_cb(btn, entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)count);
            lv_obj_add_event_cb(btn, entry_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)count);
            count++;
            entry.close();
        }
        if (entry) { // loop ended because of the cap, not the end of the directory
            truncated = true;
            entry.close();
        }
        dir.close();
    } else { // internal flash
        fs::File dir = fileSystem.open(filesPath);
        if (!dir || !dir.isDirectory()) {
            lv_list_add_text(files_list, "Can't open this folder");
            return;
        }
        fs::File entry;
        while ((entry = dir.openNextFile()) && count < kFilesMaxEntries) {
            const char *full = entry.name(); // some cores return the full path
            const char *name = strrchr(full, '/');
            name = name ? name + 1 : full;
            if (name[0] == '.') {
                entry.close();
                continue;
            }
            lv_obj_t *btn;
            filesNameIsDir[count] = entry.isDirectory();
            if (entry.isDirectory()) {
                snprintf(filesNames[count], sizeof(filesNames[0]), "/%s", name);
                btn = lv_list_add_button(files_list, LV_SYMBOL_DIRECTORY, name);
                filesStyleRow(btn, 0xffd60a);
            } else {
                snprintf(filesNames[count], sizeof(filesNames[0]), "%s", name);
                fmtBytes(entry.size(), freeBuf, sizeof(freeBuf));
                snprintf(line, sizeof(line), "%s  (%s)", name, freeBuf);
                btn = lv_list_add_button(files_list, LV_SYMBOL_FILE, line);
                filesStyleRow(btn, 0x8e8e93);
            }
            lv_obj_add_event_cb(btn, entry_cb, LV_EVENT_CLICKED, (void *)(intptr_t)count);
            lv_obj_add_event_cb(btn, entry_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)count);
            count++;
            entry.close();
        }
        if (entry) {
            truncated = true;
            entry.close();
        }
        dir.close();
    }

    if (count == 0 && !truncated)
        lv_list_add_text(files_list, "(empty)");
    if (truncated) {
        snprintf(line, sizeof(line), "... showing first %d only", kFilesMaxEntries);
        lv_list_add_text(files_list, line);
    }
#else
    lv_label_set_text(files_path_label, "Storage");
    lv_list_add_text(files_list, "No storage support in this build");
#endif
}

// -----------------------------------------------------------------------------
// Trackball double-click gesture + screen lock
//
// One gesture (double-click the trackball) does the natural thing for the state:
//   locked & dark   -> show the PIN pad to unlock
//   asleep (dimmed) -> just wake the screen (don't navigate)
//   awake, on Home  -> lock the device (black + PIN)
//   awake, in a app -> go Home to the grid
//   PIN pad showing -> cancel back to the locked/black state
// -----------------------------------------------------------------------------
void TFTView_320x240::handleHomeGesture(void)
{
    // A pins overlay (from the Maps app) must never linger over the launcher.
    if (pins_overlay)
        closePinsList();

    // FIRST, handle the screen being dark/gated — no matter WHY it went dark (manual lock,
    // idle timeout, and in any app). This is the single, deterministic wake path: relight,
    // and if a real PIN is set, always show the pad. Previously the "idle timeout" case was
    // decided separately from the "manual lock" case, which let some apps wake straight to
    // Home without the PIN. Unifying it here fixes that.
    if (tdeck_input_gated || tdeck_hold_dark) {
        tdeck_input_gated = false;
        tdeck_hold_dark = false;           // stop forcing the backlight black
        lv_display_trigger_activity(NULL); // relight the backlight
        // Waking is also where the GPS gets re-armed: after the device sleeps, nothing else
        // restarts its search, so it would stay dark until the Settings switch was toggled.
        tdeck_gps_kick();
        // A code is always in effect (built-in default 1234 until the user sets their own), so
        // any wake — a deliberate lock OR an idle timeout, in any app — brings up the pad. This
        // is the single, deterministic wake path.
        if (effectiveLockPin() != 0) {
            lockState = LOCK_ENTRY;
            showLockPad(false);
        } else {
            lockState = LOCK_NONE; // only reachable if a build makes 0 mean "no lock"
        }
        return;
    }

    // Screen is already lit and the PIN pad is up. A double-click here must NOT re-lock the
    // screen — doing that turned an impatient double-tap into an on/off toggle that felt like
    // "it took 6 taps to unlock". Instead just clear any half-typed digits so it's a fresh
    // start. The pad is left up; Jake unlocks by entering the PIN.
    if (lockState == LOCK_ENTRY) {
        lockLen = 0;
        lockDigits[0] = 0;
        updateLockDisplay();
        return;
    }

    // Lit but flagged locked-dark (shouldn't normally happen once the gate is cleared) —
    // show the pad rather than getting stuck.
    if (lockState == LOCK_DARK) {
        lockState = LOCK_ENTRY;
        lv_display_trigger_activity(NULL);
        showLockPad(false);
        return;
    }

    // Awake & unlocked. On the Home grid -> lock; anywhere else -> go Home.
    lv_display_trigger_activity(NULL);
    if (launcher_screen && lv_screen_active() == launcher_screen) {
        lockDevice();
    } else if (launcher_screen) {
        lv_screen_load_anim(launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    }
}

// Black out the screen and require the PIN to get back in.
void TFTView_320x240::lockDevice(void)
{
    lockState = LOCK_DARK;
    lockLen = 0;
    lockDigits[0] = 0;
    if (launcher_screen) // put Home under the black so unlock returns cleanly
        lv_screen_load_anim(launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    tdeck_hold_dark = true;   // LGFXDriver snaps the backlight to 0 and holds it
    tdeck_input_gated = true; // ignore touch/keyboard/roll until a double-click
}

uint32_t TFTView_320x240::effectiveLockPin(void)
{
    // The PIN is stored as a NUMBER, and 0 doubles as "no PIN set" — so 0000 can never act as a
    // real lock (indistinguishable from unset, and unset used to mean "open"). We therefore use
    // 1234 as the built-in default: an unset device locks with 1234, which the user can change
    // to their own code in Settings. (Someone who explicitly picks 0000 just gets 1234 too.)
    return db.uiConfig.pin_code == 0 ? 1234 : db.uiConfig.pin_code;
}

void TFTView_320x240::updateLockDisplay(void)
{
    char masked[9];
    for (uint8_t i = 0; i < lockLen && i < 8; i++)
        masked[i] = '*';
    masked[lockLen] = 0;
    if (lock_digits_label)
        lv_label_set_text(lock_digits_label, lockLen ? masked : "----");
}

void TFTView_320x240::showLockPad(bool setMode)
{
    lockSetMode = setMode;
    lockLen = 0;
    lockDigits[0] = 0;

    if (!lockpad_screen) {
        lockpad_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(lockpad_screen, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_clear_flag(lockpad_screen, LV_OBJ_FLAG_SCROLLABLE);

        lock_title_label = lv_label_create(lockpad_screen);
        lv_obj_set_style_text_color(lock_title_label, lv_color_hex(0x8e8e93), LV_PART_MAIN);
        lv_obj_align(lock_title_label, LV_ALIGN_TOP_MID, 0, 6);

        // Unread-message count in the pad's top-left corner (same green "N msgs" as the launcher
        // top bar) — so a glance at the locked device tells you if anything came in.
        lockpad_unread_label = lv_label_create(lockpad_screen);
        lv_obj_set_style_text_color(lockpad_unread_label, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_set_style_text_font(lockpad_unread_label, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_align(lockpad_unread_label, LV_ALIGN_TOP_LEFT, 8, 6);
        lv_label_set_text(lockpad_unread_label, "");

        lock_digits_label = lv_label_create(lockpad_screen);
        lv_obj_set_style_text_color(lock_digits_label, lv_color_hex(0x30d158), LV_PART_MAIN);
        lv_obj_set_style_text_font(lock_digits_label, &ui_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(lock_digits_label, 4, LV_PART_MAIN);
        lv_obj_align(lock_digits_label, LV_ALIGN_TOP_MID, 0, 24);

        // plain "<" / "OK" instead of symbol glyphs — the custom 20pt font's symbol
        // coverage is unverified, and a blank Enter key on a lock screen is a disaster
        static const char *const pinmap[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                             "<", "0", "OK", ""};
        lv_obj_t *pad = lv_buttonmatrix_create(lockpad_screen);
        lv_buttonmatrix_set_map(pad, pinmap);
        // dark theme + big touch targets (fills the width, ~2/3 of the height)
        lv_obj_set_size(pad, 312, 178);
        lv_obj_align(pad, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_color(pad, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pad, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_row(pad, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_column(pad, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(pad, lv_color_hex(0x1c1c1e), LV_PART_ITEMS);
        lv_obj_set_style_text_color(pad, lv_color_hex(0xffffff), LV_PART_ITEMS);
        lv_obj_set_style_text_font(pad, &ui_font_montserrat_20, LV_PART_ITEMS);
        lv_obj_set_style_radius(pad, 12, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(pad, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(pad, lv_color_hex(0x3a3a3c), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_add_event_cb(pad, lockpad_event, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_label_set_text(lock_title_label, setMode ? "Set a new PIN, then OK" : "Enter PIN");
    updateLockDisplay();
    updateUnreadMessages(); // pad may have just been created — pull in any count that already accrued
    lv_screen_load_anim(lockpad_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Alt+C shortcut from the lock pad: run the touch-screen calibration (same routine as the Settings
// button), then re-show the lock pad. This only bypasses the PIN to *reach* calibration — it does
// NOT unlock the device; the user still enters their PIN afterwards (now with a working touch).
void TFTView_320x240::startCalibrationFromLock(void)
{
    uint16_t *parameters = (uint16_t *)db.uiConfig.calibration_data.bytes;
    memset(parameters, 0, 16);                       // clear existing data so calibrate() runs the
    displaydriver->calibrate(parameters);            // interactive "tap the marker" routine (blocking)
    db.uiConfig.calibration_data.size = 16;
    controller->storeUIConfig(db.uiConfig);          // persist the fresh calibration across reboots

    // Back to the PIN pad (still locked). calibrate() drew straight to the panel BEHIND LVGL's
    // back — and since lockpad_screen is already the active screen, showLockPad()'s screen-load is
    // a no-op and LVGL sees nothing to repaint (the pad only reappeared on the next touch). So
    // explicitly invalidate everything and repaint NOW for a clean, instant handoff.
    lockState = LOCK_ENTRY;
    lockLen = 0;
    lockDigits[0] = 0;
    tdeck_hold_dark = false;
    tdeck_input_gated = false;
    showLockPad(false);
    lv_obj_invalidate(lockpad_screen); // full-screen damage: repaint over the calibration leftovers
    lv_refr_now(NULL);                 // ...and flush it to the panel immediately
}

void TFTView_320x240::lockpad_event(lv_event_t *e)
{
    lv_obj_t *pad = (lv_obj_t *)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(pad);
    const char *key = lv_buttonmatrix_get_button_text(pad, id);
    if (!key)
        return;

    if (strcmp(key, "OK") == 0) {
        THIS->submitLockPad();
    } else if (strcmp(key, "<") == 0) {
        if (THIS->lockLen > 0)
            THIS->lockDigits[--THIS->lockLen] = 0;
        THIS->updateLockDisplay();
    } else if (key[0] >= '0' && key[0] <= '9') {
        if (THIS->lockLen < 8) {
            THIS->lockDigits[THIS->lockLen++] = key[0];
            THIS->lockDigits[THIS->lockLen] = 0;
        }
        THIS->updateLockDisplay();
    }
}

void TFTView_320x240::submitLockPad(void)
{
    uint32_t entered = (uint32_t)atol(lockDigits);

    if (lockSetMode) { // choosing a new PIN from Settings
        if (lockLen < 4) {
            lv_label_set_text(lock_title_label, "Use at least 4 digits");
            lockLen = 0;
            lockDigits[0] = 0;
            updateLockDisplay();
            return;
        }
        db.uiConfig.pin_code = entered;
        controller->storeUIConfig(db.uiConfig); // persist across reboots
        lockState = LOCK_NONE;
        openSettings(); // back to Settings; PIN saved
        return;
    }

    // Unlocking (require at least one digit so a blank OK can't slip through)
    if (lockLen > 0 && entered == effectiveLockPin()) {
        lockState = LOCK_NONE;
        tdeck_hold_dark = false;
        tdeck_input_gated = false;
        lv_display_trigger_activity(NULL);
        if (launcher_screen)
            lv_screen_load_anim(launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    } else {
        lockLen = 0;
        lockDigits[0] = 0;
        lv_label_set_text(lock_title_label, "Wrong PIN — try again");
        updateLockDisplay();
    }
}

/**
 * @brief Initialize all screens and apply customizations
 *
 */
void TFTView_320x240::init_screens(void)
{
    ILOG_DEBUG("init screens...");
    state = MeshtasticView::eInitScreens;
    ui_init();
    apply_hotfix();

    activeMsgContainer = objects.messages_container;
    // setup the two channel label panels with arrays that allow indexing
    channel = {objects.channel_label0, objects.channel_label1, objects.channel_label2, objects.channel_label3,
               objects.channel_label4, objects.channel_label5, objects.channel_label6, objects.channel_label7};
    ch_label = {objects.settings_channel0_label, objects.settings_channel1_label, objects.settings_channel2_label,
                objects.settings_channel3_label, objects.settings_channel4_label, objects.settings_channel5_label,
                objects.settings_channel6_label, objects.settings_channel7_label};

    channelGroup = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
    ui_events_init();

    // build our custom launcher and boot into it (tiles open the MUI screens)
    createLauncher();
    lv_screen_load_anim(launcher_screen, LV_SCR_LOAD_ANIM_NONE, 300, 0, false);

    // re-configuration based on capabilities
    if (!displaydriver->hasLight())
        lv_obj_add_flag(objects.basic_settings_brightness_button, LV_OBJ_FLAG_HIDDEN);

#ifndef CUSTOM_TOUCH_DRIVER
    if (!displaydriver->hasTouch())
#endif
        lv_obj_add_flag(objects.basic_settings_calibration_button, LV_OBJ_FLAG_HIDDEN);

#if LV_USE_LIBINPUT
    lv_obj_clear_flag(objects.basic_settings_input_button, LV_OBJ_FLAG_HIDDEN);
#endif

#if defined(USE_I2S_BUZZER) || defined(USE_PIN_BUZZER)
    lv_obj_clear_flag(objects.basic_settings_alert_button, LV_OBJ_FLAG_HIDDEN);
    db.uiConfig.ring_tone_id = 0;
#else
    lv_obj_add_flag(objects.basic_settings_alert_button, LV_OBJ_FLAG_HIDDEN);
#endif

#ifndef USE_ROUTER_ROLE
    lv_dropdown_set_options(objects.settings_device_role_dropdown,
                            _("Client\nClient Mute\nTracker\nSensor\nTAK\nClient Hidden\nLost & Found\nTAK Tracker"));
#endif

#ifdef HAS_SDCARD
    lv_obj_clear_flag(objects.basic_settings_backup_restore_button, LV_OBJ_FLAG_HIDDEN);
#endif

    if (controller->isStandalone()) {
        lv_obj_add_flag(objects.progmode_button, LV_OBJ_FLAG_HIDDEN);
    }

    // signal scanner scale
#if defined(USE_SX127x)
    lv_label_set_text(objects.signal_scanner_rssi_scale_label, "-50\n-60\n-70\n-80\n-90\n-100\n-110\n-120\n-130\n-140\n-150");
    lv_slider_set_range(objects.rssi_slider, -150, -50);
    lv_label_set_text(objects.signal_scanner_snr_scale_label,
                      "14.0\n12.0\n10.0\n8.0\n6.0\n4.0\n2.0\n0.0\n-2.0\n-4.0\n-8.0\n-10.0\n-12.0\n-14.0\n-16.0");
    lv_obj_set_style_text_line_space(objects.signal_scanner_snr_scale_label, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_slider_set_range(objects.snr_slider, -17, 15);
#else
    lv_label_set_text(objects.signal_scanner_rssi_scale_label, "-20\n-30\n-40\n-50\n-60\n-70\n-80\n-90\n-100\n-110\n-120");
    lv_slider_set_range(objects.rssi_slider, -125, -25);
    lv_label_set_text(objects.signal_scanner_snr_scale_label,
                      "8.0\n6.0\n4.0\n2.0\n0.0\n-2.0\n-4.0\n-8.0\n-10.0\n-12.0\n-14.0\n-16.0\n-18.0");
    lv_obj_set_style_text_line_space(objects.signal_scanner_snr_scale_label, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_slider_set_range(objects.snr_slider, -20, 9);
#endif

    setInputButtonLabel();
    lv_group_focus_obj(objects.home_button);

    // remember position of top node panel button for group linked list
    lv_ll_t *lv_group_ll = &lv_group_get_default()->obj_ll;
    for (lv_obj_t **obj_i = (lv_obj_t **)_lv_ll_get_head(lv_group_ll); obj_i != NULL;
         obj_i = (lv_obj_t **)_lv_ll_get_next(lv_group_ll, obj_i)) {
        if (*obj_i == objects.node_button) {
            topNodeLL = obj_i;
            break;
        }
    }

    // user data
    objects.home_time_button->user_data = (void *)0;
    objects.home_wlan_button->user_data = (void *)0;
    objects.home_memory_button->user_data = (void *)0;

    updateFreeMem();

    screensInitialised = true;
    state = MeshtasticView::eInitDone;
    ILOG_DEBUG("TFTView_320x240 init done.");
}

/**
 * @brief set active button, panel and top panel
 *
 * @param b button to set active
 * @param p main panel to set active
 * @param tp top panel to set active
 */
void TFTView_320x240::ui_set_active(lv_obj_t *b, lv_obj_t *p, lv_obj_t *tp)
{
    if (activeButton) {
        lv_obj_set_style_border_width(activeButton, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        if (Themes::get() == Themes::eDark)
            lv_obj_set_style_bg_img_recolor_opa(activeButton, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_img_recolor(activeButton, colorGray, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_set_style_border_width(b, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor(b, colorMesh, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(b, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (activePanel) {
        lv_obj_add_flag(activePanel, LV_OBJ_FLAG_HIDDEN);
        if (activePanel == objects.messages_panel) {
            lv_obj_remove_state(objects.message_input_area, LV_STATE_FOCUSED);
            if (!lv_obj_has_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN)) {
                hideKeyboard(objects.messages_panel);
            }
            uint32_t channelOrNode = (unsigned long)activeMsgContainer->user_data;
            // remove empty messageContainer if we are leaving messages panel
            if (channelOrNode >= c_max_channels) {
                if (activeMsgContainer->spec_attr->child_cnt == 0) {
                    eraseChat(channelOrNode);
                    updateActiveChats();
                    activeMsgContainer = objects.messages_container;
                }
            }
            unreadMessages = 0; // TODO: not all messages may be actually read
            updateUnreadMessages();
        } else if (activePanel == objects.node_options_panel) {
            // we're moving away from node options panel, so save latest settings
            storeNodeOptions();
        }
    }

    lv_obj_clear_flag(p, LV_OBJ_FLAG_HIDDEN);

    if (tp) {
        if (activeTopPanel) {
            lv_obj_add_flag(activeTopPanel, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(tp, LV_OBJ_FLAG_HIDDEN);
        activeTopPanel = tp;
    }

    activeButton = b;
    activePanel = p;
    if (activePanel == objects.messages_panel) {
        lv_group_focus_obj(objects.message_input_area);
    } else if (inputdriver->hasKeyboardDevice() || inputdriver->hasEncoderDevice()) {
        setGroupFocus(activePanel);
    }

    lv_obj_add_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.msg_popup_panel, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_320x240::enterProgrammingMode(void)
{
    if (state == eEnterProgrammingMode && !db.config.bluetooth.enabled) {
        ILOG_INFO("rebooting into programming mode");
        lv_label_set_text(objects.meshtastic_url, _("Rebooting ..."));

        if (ownNode) {
            meshtastic_Config_NetworkConfig &network = THIS->db.config.network;
            if (network.wifi_enabled) {
                network.wifi_enabled = false;
                THIS->controller->sendConfig(meshtastic_Config_NetworkConfig{network});
            }
            meshtastic_Config_BluetoothConfig &bluetooth = THIS->db.config.bluetooth;
            bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
            bluetooth.fixed_pin = random(100000, 999999);
            bluetooth.enabled = true;
            THIS->controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, ownNode);
            state = MeshtasticView::eWaitingForReboot;
        }
    } else {
        state = MeshtasticView::eProgrammingMode;

        // keep this screen lit and touchable: no idle-dim, no gate, no lock — otherwise the
        // screen goes dark, touch is swallowed, and the exit button becomes unreachable.
        tdeck_prog_mode = true;
        tdeck_input_gated = false;
        tdeck_hold_dark = false;

        // hide the "T-UI" boot title; present a clear programming-mode screen instead
        if (splash_title)
            lv_obj_add_flag(splash_title, LV_OBJ_FLAG_HIDDEN);

        // title at the top
        lv_obj_clear_flag(objects.meshtastic_url, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(objects.meshtastic_url, _(">> Programming mode <<"));
        lv_obj_set_style_text_color(objects.meshtastic_url, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_font(objects.meshtastic_url, &ui_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(objects.meshtastic_url, LV_ALIGN_TOP_MID, 0, 28);

        // pairing PIN below the (centered) bluetooth icon
        lv_label_set_text_fmt(objects.firmware_label, "PIN %06d", db.config.bluetooth.fixed_pin);
        lv_obj_set_style_text_font(objects.firmware_label, &ui_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(objects.firmware_label, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_align(objects.firmware_label, LV_ALIGN_CENTER, 0, 62);

        // exit hint at the bottom (created once, reused on re-entry)
        static lv_obj_t *progHint = nullptr;
        if (!progHint) {
            progHint = lv_label_create(objects.boot_screen);
            lv_obj_set_style_text_color(progHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
            lv_obj_set_style_text_font(progHint, &ui_font_montserrat_14, LV_PART_MAIN);
        }
        lv_obj_clear_flag(progHint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(progHint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_text(progHint, "Press any key to exit\n(or tap the Bluetooth icon)");
        lv_obj_align(progHint, LV_ALIGN_BOTTOM_MID, 0, -12);

        lv_obj_add_flag(objects.boot_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.boot_logo_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(objects.bluetooth_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(objects.bluetooth_button, ui_event_BluetoothButton, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(objects.bluetooth_button, ui_event_BluetoothButton, LV_EVENT_LONG_PRESSED, NULL);

        // Touch-independent escapes: ANY keyboard key (tdeck_prog_key_exit, set by the keyboard
        // driver) OR a trackball double-click (tb_home_request) exits programming mode. The
        // launcher's own poll timer doesn't run on this screen, so add one here. Created once;
        // it only acts while tdeck_prog_mode is set, so it can't disturb normal use.
        static bool progExitTimerMade = false;
        if (!progExitTimerMade) {
            progExitTimerMade = true;
            lv_timer_create(
                [](lv_timer_t *) {
                    if (!tdeck_prog_mode)
                        return;
                    bool key = tdeck_prog_key_exit;
                    bool dbl = false;
#if defined(INPUTDRIVER_ENCODER_TYPE)
                    dbl = tb_home_request;
                    if (dbl)
                        tb_home_request = false;
#endif
                    if (key || dbl) {
                        tdeck_prog_key_exit = false;
                        THIS->exitProgrammingMode();
                    }
                },
                60, NULL);
        }
        ILOG_INFO("### MUI programming mode entered (nodeId=!%08x) ###", ownNode);
    }
}

/**
 * @brief fix quirks in the generated ui
 *
 */
void TFTView_320x240::apply_hotfix(void)
{
    // adapt screens to custom display resolution
    uint32_t h = lv_display_get_horizontal_resolution(displaydriver->getDisplay());
    uint32_t v = lv_display_get_vertical_resolution(displaydriver->getDisplay());

    // resize buttons on larger display (assuming 480x480)
    if (h > 320 && v > 320) {
        lv_obj_t *button[] = {objects.home_button,     objects.nodes_button, objects.groups_button,
                              objects.messages_button, objects.map_button,   objects.settings_button};
        for (int i = 0; i < 6; i++) {
            lv_obj_set_size(button[i], 72, 72);
        }
    }

    // fix size for 480 pixel height displays
    if (v >= 480) {
        // keyboard size limit
        lv_obj_set_size(objects.keyboard, LV_PCT(100), LV_PCT(45));

        // resize channel buttons
        buttonSize = 40;
        lv_obj_set_height(objects.channel_button0, buttonSize);
        lv_obj_set_height(objects.channel_button1, buttonSize);
        lv_obj_set_height(objects.channel_button2, buttonSize);
        lv_obj_set_height(objects.channel_button3, buttonSize);
        lv_obj_set_height(objects.channel_button4, buttonSize);
        lv_obj_set_height(objects.channel_button5, buttonSize);
        lv_obj_set_height(objects.channel_button6, buttonSize);
        lv_obj_set_height(objects.channel_button7, buttonSize);

        lv_obj_set_height(objects.chats_button, buttonSize);
    } else {
        // chat button size
        buttonSize = 36;
    }
    if (h > 400) {
        lv_obj_set_style_text_font(objects.home_qr_label, &ui_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_obj_move_foreground(objects.keyboard);
    lv_obj_add_flag(objects.detector_radar_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.detected_node_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(objects.detector_start_label, _("Start"));
    lv_obj_clear_flag(objects.detector_start_button_panel, LV_OBJ_FLAG_HIDDEN);

    lv_textarea_set_placeholder_text(objects.message_input_area, _("Enter Text ..."));
    lv_textarea_set_placeholder_text(objects.nodes_filter_name_area, _("!Enter Filter ..."));
    lv_textarea_set_placeholder_text(objects.nodes_hl_name_area, _("Enter Filter ..."));

    auto applyStyle = [](lv_obj_t *tab_buttons) {
        for (int i = 0; i < lv_obj_get_child_count(tab_buttons); i++) {
            if (tab_buttons->spec_attr->children[i]->class_p == &lv_button_class) {
                lv_obj_add_style(tab_buttons->spec_attr->children[i], &style_btn_default, LV_STATE_DEFAULT);
                lv_obj_add_style(tab_buttons->spec_attr->children[i], &style_btn_active, LV_STATE_CHECKED);
                lv_obj_add_style(tab_buttons->spec_attr->children[i], &style_btn_pressed, LV_STATE_PRESSED);
            }
        }
    };

    lv_obj_t *tab_buttons = lv_tabview_get_tab_bar(objects.node_options_tab_view);
    applyStyle(tab_buttons);
    tab_buttons = lv_tabview_get_tab_bar(objects.controller_tab_view);
    applyStyle(tab_buttons);
    tab_buttons = lv_tabview_get_tab_bar(ui_SettingsTabView);
    applyStyle(tab_buttons);

    // add event callback to to apply custom drawing for statistics table
    lv_obj_add_event_cb(objects.statistics_table, ui_event_statistics_table, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(objects.statistics_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    // statistics table item size
    int32_t width = 36;
    int32_t rows = 12;
    if (h > 320) {
        width = (h - 48 - 57) / 6;
    }
    if (v > 240) {
        rows = (v - 32) / 18;
    }
    statisticTableRows = rows;
    lv_table_set_row_count(objects.statistics_table, statisticTableRows);
    lv_table_set_column_count(objects.statistics_table, 7);
    lv_table_set_column_width(objects.statistics_table, 0, 57);
    lv_table_set_column_width(objects.statistics_table, 1, width);
    lv_table_set_column_width(objects.statistics_table, 2, width);
    lv_table_set_column_width(objects.statistics_table, 3, width);
    lv_table_set_column_width(objects.statistics_table, 4, width);
    lv_table_set_column_width(objects.statistics_table, 5, width);
    lv_table_set_column_width(objects.statistics_table, 6, width);
    // fill table heading
    lv_table_set_cell_value(objects.statistics_table, 0, 0, _("Name"));
    lv_table_set_cell_value(objects.statistics_table, 0, 1, "Tel");
    lv_table_set_cell_value(objects.statistics_table, 0, 2, "Pos");
    lv_table_set_cell_value(objects.statistics_table, 0, 3, "Inf");
    lv_table_set_cell_value(objects.statistics_table, 0, 4, "Trc");
    lv_table_set_cell_value(objects.statistics_table, 0, 5, "Nbr");
    lv_table_set_cell_value(objects.statistics_table, 0, 6, "All");

    // transform checkbox into radio button
    static lv_style_t style_radio;
    lv_style_init(&style_radio);
    lv_style_set_radius(&style_radio, LV_RADIUS_CIRCLE);

    lv_obj_add_style(objects.settings_backup_checkbox, &style_radio, LV_PART_INDICATOR);
    lv_obj_add_style(objects.settings_restore_checkbox, &style_radio, LV_PART_INDICATOR);
}

void TFTView_320x240::updateTheme(void)
{
    Themes::initStyles();
    Themes::recolorButton(objects.home_lora_button, db.config.lora.tx_enabled);
    Themes::recolorButton(objects.home_bell_button, db.uiConfig.alert_enabled || !db.silent);
    Themes::recolorButton(objects.home_location_button,
                          db.config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED);
    Themes::recolorButton(objects.home_wlan_button, db.config.network.wifi_enabled);
    Themes::recolorButton(objects.home_mqtt_button, db.module_config.mqtt.enabled);
    Themes::recolorButton(objects.home_sd_card_button, cardDetected);
    Themes::recolorButton(objects.home_memory_button, (bool)objects.home_memory_button->user_data);
    Themes::recolorText(objects.home_lora_label, db.config.lora.tx_enabled);
    Themes::recolorText(objects.home_bell_label, db.uiConfig.alert_enabled || !db.silent);
    Themes::recolorText(objects.home_location_label,
                        db.config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED);
    Themes::recolorText(objects.home_wlan_label, db.config.network.wifi_enabled);
    Themes::recolorText(objects.home_mqtt_label, db.module_config.mqtt.enabled);
    Themes::recolorText(objects.home_sd_card_label, cardDetected);
    Themes::recolorText(objects.home_memory_label, (bool)objects.home_memory_button->user_data);

    lv_opa_t opa = (Themes::get() == Themes::eDark) ? 0 : 255;
    lv_obj_set_style_bg_img_recolor_opa(objects.home_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(objects.nodes_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(objects.groups_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(objects.messages_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(objects.map_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_img_recolor_opa(objects.settings_button, opa, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int i = 0; i < c_max_channels; i++) {
        if (db.channel[i].role != meshtastic_Channel_Role_DISABLED)
            updateGroupChannel(i);
    }
}

void TFTView_320x240::ui_events_init(void)
{
    // just a test to implement callback via non-static lambda function
    auto ui_event_HomeButton = [](lv_event_t *e) {
        lv_event_code_t event_code = lv_event_get_code(e);
        if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
            TFTView_320x240 &view = *static_cast<TFTView_320x240 *>(e->user_data);
            view.ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
        } else if (event_code == LV_EVENT_LONG_PRESSED) {
            if (THIS->MeshtasticView::getState() >= THIS->MeshtasticView::eConfigComplete) {
                // force re-sync with node
                THIS->controller->setConfigRequested(true);
                THIS->notifyResync(true);
            }
        }
    };

    // main button events
    lv_obj_add_event_cb(objects.home_button, ui_event_HomeButton, LV_EVENT_ALL, this); // uses lambda above
    lv_obj_add_event_cb(objects.nodes_button, this->ui_event_NodesButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.groups_button, this->ui_event_GroupsButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.messages_button, this->ui_event_MessagesButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.map_button, this->ui_event_MapButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.settings_button, this->ui_event_SettingsButton, LV_EVENT_ALL, NULL);

    // home buttons
    lv_obj_add_event_cb(objects.home_mail_button, this->ui_event_EnvelopeButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.home_nodes_button, this->ui_event_OnlineNodesButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.home_time_button, this->ui_event_TimeButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.home_lora_button, this->ui_event_LoRaButton, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(objects.home_bell_button, this->ui_event_BellButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.home_location_button, this->ui_event_LocationButton, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(objects.home_wlan_button, this->ui_event_WLANButton, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(objects.home_mqtt_button, this->ui_event_MQTTButton, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.home_sd_card_button, this->ui_event_SDCardButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.home_memory_button, this->ui_event_MemoryButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.home_qr_button, this->ui_event_QrButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.home_cancel_qr_button, this->ui_event_CancelQrButton, LV_EVENT_CLICKED, NULL);

    // node and channel buttons
    lv_obj_add_event_cb(objects.node_button, ui_event_NodeButton, LV_EVENT_ALL, (void *)ownNode);

    // 8 channel buttons
    lv_obj_add_event_cb(objects.channel_button0, ui_event_ChannelButton, LV_EVENT_ALL, (void *)0);
    lv_obj_add_event_cb(objects.channel_button1, ui_event_ChannelButton, LV_EVENT_ALL, (void *)1);
    lv_obj_add_event_cb(objects.channel_button2, ui_event_ChannelButton, LV_EVENT_ALL, (void *)2);
    lv_obj_add_event_cb(objects.channel_button3, ui_event_ChannelButton, LV_EVENT_ALL, (void *)3);
    lv_obj_add_event_cb(objects.channel_button4, ui_event_ChannelButton, LV_EVENT_ALL, (void *)4);
    lv_obj_add_event_cb(objects.channel_button5, ui_event_ChannelButton, LV_EVENT_ALL, (void *)5);
    lv_obj_add_event_cb(objects.channel_button6, ui_event_ChannelButton, LV_EVENT_ALL, (void *)6);
    lv_obj_add_event_cb(objects.channel_button7, ui_event_ChannelButton, LV_EVENT_ALL, (void *)7);

    // message popup
    lv_obj_add_event_cb(objects.msg_popup_button, this->ui_event_MsgPopupButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.msg_popup_panel, this->ui_event_MsgPopupButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.msg_restore_button, this->ui_event_MsgRestoreButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.msg_restore_panel, this->ui_event_MsgRestoreButton, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.alert_panel, this->ui_event_AlertButton, LV_EVENT_CLICKED, NULL);

    // keyboard
    lv_obj_add_event_cb(objects.keyboard, ui_event_Keyboard, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(objects.keyboard_button_0, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)0);
    lv_obj_add_event_cb(objects.keyboard_button_1, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)1);
    lv_obj_add_event_cb(objects.keyboard_button_2, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)2);
    lv_obj_add_event_cb(objects.keyboard_button_3, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)3);
    lv_obj_add_event_cb(objects.keyboard_button_4, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)4);
    lv_obj_add_event_cb(objects.keyboard_button_5, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)5);
    lv_obj_add_event_cb(objects.keyboard_button_6, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)6);
    lv_obj_add_event_cb(objects.keyboard_button_7, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)7);
    lv_obj_add_event_cb(objects.keyboard_button_8, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)8);
    lv_obj_add_event_cb(objects.keyboard_button_9, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)9);
    lv_obj_add_event_cb(objects.keyboard_button_10, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)10);
    lv_obj_add_event_cb(objects.keyboard_button_11, ui_event_KeyboardButton, LV_EVENT_CLICKED, (void *)11);

    // message text area
    lv_obj_add_event_cb(objects.message_input_area, ui_event_message_ready, LV_EVENT_ALL, NULL);

    // basic settings buttons
    lv_obj_add_event_cb(objects.basic_settings_user_button, ui_event_user_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_role_button, ui_event_role_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_region_button, ui_event_region_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_modem_preset_button, ui_event_preset_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_wifi_button, ui_event_wifi_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_language_button, ui_event_language_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_channel_button, ui_event_channel_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_timeout_button, ui_event_timeout_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_screen_lock_button, ui_event_screen_lock_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_brightness_button, ui_event_brightness_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_theme_button, ui_event_theme_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_calibration_button, ui_event_calibration_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_input_button, ui_event_input_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_alert_button, ui_event_alert_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_backup_restore_button, ui_event_backup_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_reset_button, ui_event_reset_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.basic_settings_reboot_button, ui_event_reboot_button, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(objects.reboot_button, ui_event_device_reboot_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.progmode_button, ui_event_device_progmode_button, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.shutdown_button, ui_event_device_shutdown_button, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.cancel_reboot_button, ui_event_device_cancel_button, LV_EVENT_CLICKED, NULL);

    // sliders
    lv_obj_add_event_cb(objects.screen_timeout_slider, ui_event_screen_timeout_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.brightness_slider, ui_event_brightness_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.frequency_slot_slider, ui_event_frequency_slot_slider, LV_EVENT_VALUE_CHANGED, NULL);

    // dropdown
    lv_obj_add_event_cb(objects.settings_modem_preset_dropdown, ui_event_modem_preset_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.setup_region_dropdown, ui_event_setup_region_dropdown, LV_EVENT_VALUE_CHANGED, NULL);

    // OK / Cancel widget for basic settings dialog
    lv_obj_add_event_cb(objects.obj2__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj2__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj3__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj3__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj4__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj4__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj5__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj5__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj6__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj6__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj7__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj7__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj8__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj8__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj9__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj9__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj10__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj10__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj11__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj11__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj12__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj12__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj13__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj13__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj14__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj14__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj15__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj15__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj16__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj16__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj17__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj17__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj18__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj18__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj21__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj21__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj27__ok_button_w, ui_event_ok, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.obj27__cancel_button_w, ui_event_cancel, LV_EVENT_CLICKED, 0);

    // modify channel buttons
    lv_obj_add_event_cb(objects.settings_channel0_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)0);
    lv_obj_add_event_cb(objects.settings_channel1_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)1);
    lv_obj_add_event_cb(objects.settings_channel2_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)2);
    lv_obj_add_event_cb(objects.settings_channel3_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)3);
    lv_obj_add_event_cb(objects.settings_channel4_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)4);
    lv_obj_add_event_cb(objects.settings_channel5_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)5);
    lv_obj_add_event_cb(objects.settings_channel6_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)6);
    lv_obj_add_event_cb(objects.settings_channel7_button, ui_event_modify_channel, LV_EVENT_ALL, (void *)7);
    // delete channel button
    lv_obj_add_event_cb(objects.settings_modify_trash_button, ui_event_delete_channel, LV_EVENT_CLICKED, NULL);
    // generate PSK
    lv_obj_add_event_cb(objects.settings_modify_channel_key_generate_button, ui_event_generate_psk, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.settings_modify_channel_qr_button, ui_event_qr_code, LV_EVENT_CLICKED, NULL);

    // screen
    lv_obj_add_event_cb(objects.calibration_screen, ui_event_calibration_screen_loaded, LV_EVENT_SCREEN_LOADED, (void *)7);
    lv_obj_add_event_cb(objects.screen_lock_button_matrix, ui_event_pin_screen_button, LV_EVENT_ALL, 0);

    lv_obj_add_event_cb(objects.settings_backup_checkbox, ui_event_backup_restore_radio_button, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.settings_restore_checkbox, ui_event_backup_restore_radio_button, LV_EVENT_ALL, NULL);

    // map settings and navigation
    lv_obj_add_event_cb(objects.main_screen, ui_screen_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(objects.arrow_up_button, ui_event_arrow, LV_EVENT_CLICKED, (void *)8);
    lv_obj_add_event_cb(objects.arrow_left_button, ui_event_arrow, LV_EVENT_CLICKED, (void *)4);
    lv_obj_add_event_cb(objects.arrow_right_button, ui_event_arrow, LV_EVENT_CLICKED, (void *)6);
    lv_obj_add_event_cb(objects.arrow_down_button, ui_event_arrow, LV_EVENT_CLICKED, (void *)2);
    lv_obj_add_event_cb(objects.nav_button, ui_event_navHome, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(objects.zoom_slider, ui_event_zoomSlider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.zoom_in_button, ui_event_zoomIn, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.zoom_out_button, ui_event_zoomOut, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.gps_lock_button, ui_event_lockGps, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(objects.map_brightness_slider, ui_event_mapBrightnessSlider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.map_contrast_slider, ui_event_mapContrastSlider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.map_style_dropdown, ui_event_map_style_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(objects.map_url_dropdown, ui_event_map_url_dropdown, LV_EVENT_VALUE_CHANGED, NULL);

    // tools buttons
    lv_obj_add_event_cb(objects.tools_mesh_detector_button, ui_event_mesh_detector, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.tools_signal_scanner_button, ui_event_signal_scanner, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.tools_trace_route_button, ui_event_trace_route, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.tools_neighbors_button, ui_event_node_details, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.tools_statistics_button, ui_event_statistics, LV_EVENT_ALL, 0);
    lv_obj_add_event_cb(objects.tools_packet_log_button, ui_event_packet_log, LV_EVENT_ALL, 0);
    // tools
    lv_obj_add_event_cb(objects.detector_start_button, ui_event_mesh_detector_start, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.signal_scanner_node_button, ui_event_signal_scanner_node, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.signal_scanner_start_button, ui_event_signal_scanner_start, LV_EVENT_ALL, 0);
    lv_obj_add_event_cb(objects.trace_route_to_button, ui_event_trace_route_to, LV_EVENT_CLICKED, 0);
    lv_obj_add_event_cb(objects.trace_route_start_button, ui_event_trace_route_start, LV_EVENT_CLICKED, 0);
}

#if 0 // defined above as lambda function for tests
void TDeckGUI::ui_event_HomeButton(lv_event_t * e) {
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        TDeckGUI::instance()->ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
    }
}
#endif

void TFTView_320x240::timer_event_reboot(lv_timer_t *timer)
{
    ILOG_INFO("Rebooting...");
    THIS->controller->stop();
    delay(4000);
#if defined(ARCH_PORTDUINO)
    extern void reboot();
    reboot();
#elif defined(ARCH_ESP32)
    esp_restart();
#else
    // TODO: implement for other platforms
#endif
}

void TFTView_320x240::timer_event_shutdown(lv_timer_t *timer)
{
    ILOG_INFO("Shutdown...");
    THIS->controller->stop();
    delay(1000);
#if defined(ARCH_PORTDUINO)
    exit(2);
#elif defined(ARCH_ESP32)
    esp_deep_sleep_start();
#else
    // TODO: implement for other platforms
#endif
}

void TFTView_320x240::timer_event_programming_mode(lv_timer_t *timer)
{
    if (THIS->state == eBooting)
        THIS->state = MeshtasticView::eBootScreenDone;
    else if (THIS->state == eHoldingBootLogo) {
        lv_obj_add_flag(objects.boot_logo_arc, LV_OBJ_FLAG_HIDDEN);
        THIS->state = MeshtasticView::eEnterProgrammingMode;
        THIS->enterProgrammingMode();
    }
    lv_obj_remove_event_cb(objects.boot_logo_button, ui_event_LogoButton);
}

void TFTView_320x240::ui_event_LogoButton(lv_event_t *e)
{

    static uint32_t start = 0;
    static lv_anim_t anim;
    static auto animCB = [](void *var, int32_t v) { lv_arc_set_bg_end_angle((lv_obj_t *)var, v); };

    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_anim_del(&objects.boot_logo_arc, animCB);
        lv_obj_add_flag(objects.boot_logo_arc, LV_OBJ_FLAG_HIDDEN);
        if (millis() - start > 800) {
            THIS->state = MeshtasticView::eEnterProgrammingMode;
            THIS->enterProgrammingMode();
        } else {
            lv_obj_add_flag(objects.boot_logo_arc, LV_OBJ_FLAG_HIDDEN);
            THIS->state = MeshtasticView::eBootScreenDone;
        }
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        if (THIS->state != MeshtasticView::eHoldingBootLogo) {
            THIS->state = MeshtasticView::eHoldingBootLogo;
            start = millis();

            lv_obj_clear_flag(objects.boot_logo_arc, LV_OBJ_FLAG_HIDDEN);
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, objects.boot_logo_arc);
            lv_anim_set_values(&anim, 0, 360);
            lv_anim_set_duration(&anim, 800);
            lv_anim_set_exec_cb(&anim, animCB);
            lv_anim_set_path_cb(&anim, lv_anim_path_linear);
            lv_anim_start(&anim);
        }
    }
}

// Leave programming mode: turn Bluetooth off (which drops the PIN screen) and let the
// device reboot to the launcher. Reachable two independent ways — a touchscreen tap on the
// Bluetooth icon, OR a trackball double-click (different hardware, so it still works even if
// touch isn't reaching this screen). Idempotent-ish: guarded by tdeck_prog_mode.
void TFTView_320x240::exitProgrammingMode(void)
{
    if (!tdeck_prog_mode)
        return; // already leaving / not in programming mode
    ILOG_INFO("leaving programming mode");
    tdeck_prog_mode = false; // normal dim/lock behavior resumes
    lv_label_set_text(objects.meshtastic_url, _("Rebooting ..."));
    lv_label_set_text(objects.firmware_label, "");
    lv_obj_remove_flag(objects.boot_logo_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.bluetooth_button, LV_OBJ_FLAG_HIDDEN);

    meshtastic_Config_BluetoothConfig &bluetooth = THIS->db.config.bluetooth;
    bluetooth.enabled = false;
    THIS->controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, THIS->ownNode);
}

void TFTView_320x240::ui_event_BluetoothButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    // A simple tap OR a long press exits.
    if (event_code == LV_EVENT_CLICKED || event_code == LV_EVENT_LONG_PRESSED)
        THIS->exitProgrammingMode();
}

void TFTView_320x240::ui_event_NodesButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    static bool filterNeedsUpdate = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
        if (filterNeedsUpdate) {
            THIS->updateNodesFiltered(true);
            THIS->updateNodesStatus();
            lv_obj_scroll_to_view(objects.node_panel, LV_ANIM_ON);
            if (THIS->map) {
                THIS->map->forceRedraw(true);
            }
            filterNeedsUpdate = false;
        }
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        filterNeedsUpdate = true;
        ignoreClicked = true;
        THIS->ui_set_active(objects.nodes_button, objects.node_options_panel, objects.top_node_options_panel);
    }
}

void TFTView_320x240::ui_event_NodeButton(lv_event_t *e)
{
    static bool animRunning = false;
    static auto deleted_cb = [](_lv_anim_t *) { animRunning = false; };
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && !animRunning) {
        uint32_t nodeNum = (unsigned long)e->user_data;
        if (!nodeNum) // event-handler for own node has value 0 in user_data
            nodeNum = THIS->ownNode;
        lv_obj_t *panel = THIS->nodes[nodeNum];
        if (currentPanel) {
            // create animation to shrink other panel
            animRunning = true;
            static lv_anim_t a;
            int32_t height = lv_obj_get_height(currentPanel);
            lv_anim_init(&a);
            lv_anim_set_var(&a, currentPanel);
            lv_anim_set_values(&a, height, 136 - height);
            lv_anim_set_duration(&a, 200);
            lv_anim_set_exec_cb(&a, ui_anim_node_panel_cb);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_set_deleted_cb(&a, deleted_cb);
            lv_anim_start(&a);
        }
        if (panel != currentPanel) {
            // create animation to enlarge node panel
            animRunning = true;
            static lv_anim_t a;
            int32_t height = lv_obj_get_height(panel);
            lv_anim_init(&a);
            lv_anim_set_var(&a, panel);
            lv_anim_set_values(&a, height, 136 - height);
            lv_anim_set_duration(&a, 200);
            lv_anim_set_exec_cb(&a, ui_anim_node_panel_cb);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_set_deleted_cb(&a, deleted_cb);
            lv_anim_start(&a);
            currentPanel = panel;
            currentNode = nodeNum;
        } else {
            currentPanel = nullptr;
            currentNode = 0;
        }
        if (THIS->chooseNodeSignalScanner) {
            THIS->chooseNodeSignalScanner = false;
            ui_event_signal_scanner(NULL);
            // restore previous filter
            lv_dropdown_set_selected(objects.nodes_filter_hops_dropdown, THIS->selectedHops);
            THIS->updateNodesFiltered(true);
            THIS->updateNodesStatus();
        } else if (THIS->chooseNodeTraceRoute) {
            THIS->chooseNodeTraceRoute = false;
            ui_event_trace_route(NULL);
        }
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        //  set color and text of clicked node
        uint32_t nodeNum = (unsigned long)e->user_data;
        bool isMessagable = !((unsigned long)(THIS->nodes[nodeNum]->LV_OBJ_IDX(node_img_idx)->user_data) == eRole::unmessagable);
        if (nodeNum != THIS->ownNode && isMessagable)
            THIS->showMessages(nodeNum);
    }
}

void TFTView_320x240::ui_event_GroupsButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        THIS->ui_set_active(objects.groups_button, objects.groups_panel, objects.top_groups_panel);
    }
}

void TFTView_320x240::ui_event_ChannelButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        uint8_t ch = (uint8_t)(unsigned long)e->user_data;
        if (THIS->db.channel[ch].role != meshtastic_Channel_Role_DISABLED) {
            if (THIS->messagesRestored) {
                THIS->showMessages(ch);
            } else {
                lv_obj_clear_flag(objects.msg_restore_panel, LV_OBJ_FLAG_HIDDEN);
                lv_group_focus_obj(objects.msg_restore_button);
            }
        }
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        // toggle mute channel
        uint8_t ch = (uint8_t)(unsigned long)e->user_data;
        bool mute = THIS->db.channel[ch].settings.module_settings.is_muted;
        THIS->db.channel[ch].settings.module_settings.is_muted = !mute;
        THIS->updateChannelConfig(THIS->db.channel[ch]);
        THIS->controller->sendConfig(THIS->db.channel[ch], THIS->ownNode);
        ignoreClicked = true;
    }
}

void TFTView_320x240::ui_event_MessagesButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        if (THIS->messagesRestored) {
            THIS->ui_set_active(objects.messages_button, objects.chats_panel, objects.top_chats_panel);
        } else {
            lv_obj_clear_flag(objects.msg_restore_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.msg_restore_button);
        }
    }
}

void TFTView_320x240::ui_event_MapButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        if (THIS->activePanel == objects.map_panel) {
            // toggle navigation and zoom slider
            static bool toggle = true;
            toggle = !toggle;
            if (toggle) {
                // lv_obj_clear_flag(objects.zoom_slider, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(objects.gps_lock_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(objects.zoom_in_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(objects.zoom_out_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(objects.navigation_panel, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(objects.zoom_slider, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(objects.gps_lock_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(objects.zoom_in_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(objects.zoom_out_button, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(objects.navigation_panel, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            THIS->ui_set_active(objects.map_button, objects.map_panel, objects.top_map_panel);
            THIS->loadMap();
            lv_group_focus_obj(objects.nav_button);
        }
        lv_obj_add_flag(objects.map_osd_panel, LV_OBJ_FLAG_HIDDEN);
    } else if (event_code == LV_EVENT_LONG_PRESSED && THIS->activeSettings == eNone) {
        lv_obj_clear_flag(objects.map_osd_panel, LV_OBJ_FLAG_HIDDEN);
        ignoreClicked = true;
    }
}

void TFTView_320x240::ui_event_SettingsButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    static bool advancedMode = false;
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        if (THIS->db.uiConfig.settings_lock) {
            lv_obj_add_flag(objects.tab_page_basic_settings, LV_OBJ_FLAG_HIDDEN);
            THIS->ui_set_active(objects.settings_button, objects.controller_panel, objects.top_settings_panel);
            lv_screen_load_anim(objects.lock_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        } else {
            THIS->ui_set_active(objects.settings_button, objects.controller_panel, objects.top_settings_panel);
        }
    } else if (event_code == LV_EVENT_LONG_PRESSED && !advancedMode && THIS->activeSettings == eNone) {
        ILOG_DEBUG("screen locked");
        screenLocked = true;
        screenUnlockRequest = false;
        ignoreClicked = true;
    } else if (event_code == LV_EVENT_LONG_PRESSED && advancedMode && THIS->activeSettings == eNone) {
        advancedMode = !advancedMode;
        THIS->ui_set_active(objects.settings_button, ui_AdvancedSettingsPanel, objects.top_advanced_settings_panel);
    }
}

void TFTView_320x240::ui_event_ChatButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (event_code == LV_EVENT_LONG_PRESSED) {
        ignoreClicked = true;
        lv_obj_t *delBtn = target->LV_OBJ_IDX(1);
        lv_obj_clear_flag(delBtn, LV_OBJ_FLAG_HIDDEN);
    } else if (event_code == LV_EVENT_DEFOCUSED || event_code == LV_EVENT_LEAVE) {
        lv_obj_t *delBtn = target->LV_OBJ_IDX(1);
        lv_obj_add_flag(delBtn, LV_OBJ_FLAG_HIDDEN);
    } else if (event_code == LV_EVENT_CLICKED) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        lv_obj_set_style_border_color(target, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);

        uint32_t channelOrNode = (unsigned long)e->user_data;
        if (channelOrNode < c_max_channels) {
            uint8_t ch = (uint8_t)channelOrNode;
            THIS->showMessages(ch);
            THIS->ui_set_active(objects.messages_button, objects.messages_panel, objects.top_group_chat_panel);
        } else {
            uint32_t nodeNum = channelOrNode;
            THIS->showMessages(nodeNum);
            THIS->ui_set_active(objects.messages_button, objects.messages_panel, objects.top_messages_panel);
        }
    }
}

/**
 * @brief Del button pressed, handle deletion or clearance of chat and messages panel
 *
 * @param e
 */
void TFTView_320x240::ui_event_ChatDelButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_obj_t *target = lv_event_get_target_obj(e);
        lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);

        uint32_t channelOrNode = (unsigned long)e->user_data;
        if (channelOrNode < c_max_channels) {
            THIS->eraseChat(channelOrNode);
            THIS->controller->removeTextMessages(THIS->ownNode, UINT32_MAX, channelOrNode);
        } else {
            THIS->eraseChat(channelOrNode);
            THIS->applyNodesFilter(channelOrNode);
            THIS->controller->removeTextMessages(THIS->ownNode, channelOrNode, 0);
        }
        THIS->activeMsgContainer = objects.messages_container;
        THIS->updateActiveChats();
        if (THIS->chats.empty()) {
            // last chat was deleted, now we can get rid of all logs :)
            THIS->controller->removeTextMessages(0, 0, 0);
        }
    }
}

/**
 * @brief hide msgPopupPanel on touch; goto message on button press
 *
 */
void TFTView_320x240::ui_event_MsgPopupButton(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (target == objects.msg_popup_panel) {
        THIS->hideMessagePopup();
    } else { // msg button was clicked
        uint32_t channelOrNode = (unsigned long)objects.msg_popup_button->user_data;
        if (channelOrNode < c_max_channels) {
            uint8_t ch = (uint8_t)channelOrNode;
            THIS->showMessages(ch);
        } else {
            uint32_t nodeNum = channelOrNode;
            THIS->showMessages(nodeNum);
        }
    }
}

/**
 * @brief hide msgRestorePanel on touch
 *
 */
void TFTView_320x240::ui_event_MsgRestoreButton(lv_event_t *e)
{
    lv_obj_add_flag(objects.msg_restore_panel, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_320x240::ui_event_EnvelopeButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        if (!THIS->messagesRestored) {
            lv_obj_clear_flag(objects.msg_restore_panel, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (THIS->configComplete)
            THIS->ui_set_active(objects.messages_button, objects.chats_panel, objects.top_chats_panel);
    }
}

void TFTView_320x240::ui_event_AlertButton(lv_event_t *e)
{
    lv_obj_add_flag(objects.alert_panel, LV_OBJ_FLAG_HIDDEN);
}

void TFTView_320x240::ui_event_OnlineNodesButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->configComplete) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        lv_obj_set_state(objects.nodes_filter_offline_switch, LV_STATE_CHECKED, true);
        THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
        THIS->updateNodesFiltered(true);
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        // reset all filters
        lv_obj_set_state(objects.nodes_filter_unknown_switch, LV_STATE_CHECKED, false);
        lv_obj_set_state(objects.nodes_filter_offline_switch, LV_STATE_CHECKED, false);
        lv_obj_set_state(objects.nodes_filter_public_key_switch, LV_STATE_CHECKED, false);
        lv_obj_set_state(objects.nodes_filter_position_switch, LV_STATE_CHECKED, false);
        lv_dropdown_set_selected(objects.nodes_filter_channel_dropdown, 0);
        lv_dropdown_set_selected(objects.nodes_filter_hops_dropdown, 0);
        lv_textarea_set_text(objects.nodes_filter_name_area, "");
        THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
        THIS->updateNodesFiltered(true);
        THIS->storeNodeOptions();
        ignoreClicked = true;
    }
}

void TFTView_320x240::ui_event_TimeButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        // toggle date/time <-> uptime display
        uint32_t toggle = (unsigned long)objects.home_time_button->user_data;
        objects.home_time_button->user_data = (void *)(1 - toggle);
        THIS->updateTime();
    }
}

void TFTView_320x240::ui_event_LoRaButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_LONG_PRESSED && THIS->db.config.has_lora) {
        // toggle lora tx on/off
        meshtastic_Config_LoRaConfig &lora = THIS->db.config.lora;
        lora.tx_enabled = !lora.tx_enabled;
        THIS->controller->sendConfig(meshtastic_Config_LoRaConfig{lora});
        THIS->showLoRaFrequency(lora);
    }
}

void TFTView_320x240::ui_event_BellButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->db.module_config.has_external_notification) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        // set banner and sound on
        if (THIS->db.silent && (bool)objects.home_bell_button->user_data) {
            if (THIS->db.uiConfig.ring_tone_id == 0) {
                THIS->db.uiConfig.ring_tone_id = 1;
            }
            THIS->db.silent = false;
            THIS->db.uiConfig.alert_enabled = true;
            THIS->controller->sendConfig(ringtone[THIS->db.uiConfig.ring_tone_id].rtttl, THIS->ownNode);
            objects.home_bell_button->user_data = (void *)false;
        }
        // toggle sound only
        else if (THIS->db.uiConfig.alert_enabled && !THIS->db.silent) {
            if (THIS->db.uiConfig.ring_tone_id == 0) {
                THIS->db.uiConfig.ring_tone_id = 1;
            }
            THIS->db.uiConfig.alert_enabled = false;
            THIS->controller->sendConfig(ringtone[THIS->db.uiConfig.ring_tone_id].rtttl, THIS->ownNode);
        }
        // toggle banner only
        else if (!THIS->db.uiConfig.alert_enabled && !THIS->db.silent) {
            THIS->db.silent = true;
            THIS->db.uiConfig.alert_enabled = true;
            THIS->controller->sendConfig(ringtone[0].rtttl, THIS->ownNode);
        }
        // toggle banner & sound
        else {
            if (THIS->db.uiConfig.ring_tone_id == 0) {
                THIS->db.uiConfig.ring_tone_id = 1;
            }
            THIS->db.silent = false;
            THIS->db.uiConfig.alert_enabled = true;
            THIS->controller->sendConfig(ringtone[THIS->db.uiConfig.ring_tone_id].rtttl, THIS->ownNode);
        }
        THIS->setBellText(THIS->db.uiConfig.alert_enabled, !THIS->db.silent);
        THIS->controller->storeUIConfig(THIS->db.uiConfig);
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        ignoreClicked = true;
        if ((bool)objects.home_bell_button->user_data) {
            if (THIS->db.uiConfig.ring_tone_id == 0) {
                THIS->db.uiConfig.ring_tone_id = 1;
            }
            THIS->db.silent = false;
            THIS->db.uiConfig.alert_enabled = true;
            THIS->controller->sendConfig(ringtone[THIS->db.uiConfig.ring_tone_id].rtttl, THIS->ownNode);
            objects.home_bell_button->user_data = (void *)false;
        } else {
            THIS->db.silent = true;
            THIS->db.uiConfig.alert_enabled = false;
            THIS->controller->sendConfig(ringtone[0].rtttl, THIS->ownNode);
            objects.home_bell_button->user_data = (void *)true;
        }
        THIS->setBellText(THIS->db.uiConfig.alert_enabled, !THIS->db.silent);
        THIS->controller->storeUIConfig(THIS->db.uiConfig);
    }
}

void TFTView_320x240::ui_event_LocationButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_PRESSED && THIS->configComplete) {
        // TODO: figure out if there is a way to enabled GPS without a reboot (ala triple-click)
        // over phone api and switch between enabled/disabled with short press
        // uint32_t toggle = (unsigned long)objects.home_location_button->user_data;
        // objects.home_location_button->user_data = (void *)(1 - toggle);
        // Themes::recolorButton(objects.home_location_button, toggle);
    } else if (event_code == LV_EVENT_LONG_PRESSED && THIS->configComplete) {
        // toggle GPS not_present <-> enabled
        uint32_t toggle = (unsigned long)objects.home_location_button->user_data;
        objects.home_location_button->user_data = (void *)(1 - toggle);

        meshtastic_Config_PositionConfig &position = THIS->db.config.position;
        if (position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT)
            position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
        else {
            position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
        }
        Themes::recolorButton(objects.home_location_button,
                              position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED);
        THIS->controller->sendConfig(meshtastic_Config_PositionConfig{position});
        THIS->notifyReboot(true);
    }
}

void TFTView_320x240::ui_event_WLANButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_LONG_PRESSED && THIS->db.config.has_network) {
        if ((THIS->db.config.network.wifi_ssid[0] == '\0' || THIS->db.config.network.wifi_psk[0] == '\0') &&
            !THIS->db.connectionStatus.wifi.status.is_connected &&
            !THIS->db.config.network.eth_enabled) { // TODO: this is a workaround for bug in portduino layer
            // open settings dialog
            lv_textarea_set_text(objects.settings_wifi_ssid_textarea, THIS->db.config.network.wifi_ssid);
            lv_textarea_set_text(objects.settings_wifi_password_textarea, THIS->db.config.network.wifi_psk);
            lv_obj_clear_flag(objects.settings_wifi_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.settings_wifi_ssid_textarea);
            THIS->disablePanel(objects.home_panel);
            lv_obj_clear_state(objects.home_wlan_button, LV_STATE_PRESSED);
            THIS->activeSettings = eWifi;
        } else {
            // toggle WLAN on/off
            uint32_t toggle = (unsigned long)objects.home_wlan_button->user_data;
            objects.home_wlan_button->user_data = (void *)(1 - toggle);
            meshtastic_Config_NetworkConfig &network = THIS->db.config.network;
            network.wifi_enabled = !network.wifi_enabled;
            Themes::recolorButton(objects.home_wlan_button, network.wifi_enabled);
            THIS->controller->sendConfig(meshtastic_Config_NetworkConfig{network});
            THIS->notifyReboot(true);
        }
    }
}

void TFTView_320x240::ui_event_MQTTButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_LONG_PRESSED && THIS->configComplete) {
        // toggle MQTT on/off
        uint32_t toggle = (unsigned long)objects.home_mqtt_button->user_data;
        objects.home_mqtt_button->user_data = (void *)(1 - toggle);

        meshtastic_ModuleConfig_MQTTConfig &mqtt = THIS->db.module_config.mqtt;
        mqtt.enabled = !mqtt.enabled;
        Themes::recolorButton(objects.home_mqtt_button, mqtt.enabled);
        THIS->controller->sendConfig(meshtastic_ModuleConfig_MQTTConfig{mqtt});
        THIS->notifyReboot(true);
    }
}

void TFTView_320x240::ui_event_SDCardButton(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        THIS->updateSDCard();
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        if (THIS->formatSD) {
            ignoreClicked = true;
            THIS->formatSDCard();
        }
    }
}

void TFTView_320x240::ui_event_MemoryButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        // toggle memory display updates
        uint32_t toggle = (unsigned long)objects.home_memory_button->user_data;
        objects.home_memory_button->user_data = (void *)(1 - toggle);
        Themes::recolorButton(objects.home_memory_button, !toggle);
        Themes::recolorText(objects.home_memory_label, !toggle);
        if ((unsigned long)objects.home_memory_button->user_data) {
            THIS->updateFreeMem();
        }
    }
}

void TFTView_320x240::ui_event_QrButton(lv_event_t *e)
{
    meshtastic_SharedContact contact{.node_num = THIS->ownNode, .has_user = true, .user = THIS->db.user, .should_ignore = false};

    meshtastic_Data_payload_t payload;
    payload.size = pb_encode_to_bytes(payload.bytes, sizeof(payload.bytes), &meshtastic_SharedContact_msg, &contact);
    std::string base64Https = THIS->pskToBase64(payload.bytes, payload.size);
    for (char &c : base64Https) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
        else if (c == '=')
            c = '\0';
    }
    std::string qr = "https://meshtastic.org/v/#" + base64Https;
    lv_obj_remove_flag(objects.home_show_qr_panel, LV_OBJ_FLAG_HIDDEN);
    THIS->qr = THIS->showQrCode(objects.home_show_qr_panel, qr.c_str());
}

void TFTView_320x240::ui_event_CancelQrButton(lv_event_t *e)
{
    lv_obj_add_flag(objects.home_show_qr_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(THIS->qr);
    THIS->qr = nullptr;
}

void TFTView_320x240::ui_event_BlankScreenButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        ILOG_DEBUG("screen unlocked by button");
        screenUnlockRequest = true;
    }
}

void TFTView_320x240::ui_event_KeyboardButton(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        uint32_t keyBtnIdx = (unsigned long)e->user_data;
        switch (keyBtnIdx) {
        case 0:
            if (lv_obj_has_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN);
                THIS->showKeyboard(objects.message_input_area);
            } else {
                THIS->hideKeyboard(objects.messages_panel);
            }
            lv_group_focus_obj(objects.message_input_area);
            return; // continue play animation, don't hide keyboard immediately
        case 1:
            THIS->showKeyboard(objects.settings_user_short_textarea);
            lv_group_focus_obj(objects.settings_user_short_textarea);
            break;
        case 2:
            THIS->showKeyboard(objects.settings_user_long_textarea);
            lv_group_focus_obj(objects.settings_user_long_textarea);
            break;
        case 3:
            THIS->showKeyboard(objects.settings_modify_channel_name_textarea);
            lv_group_focus_obj(objects.settings_modify_channel_name_textarea);
            break;
        case 4:
            THIS->showKeyboard(objects.settings_modify_channel_psk_textarea);
            lv_group_focus_obj(objects.settings_modify_channel_psk_textarea);
            break;
        case 5:
            THIS->showKeyboard(objects.nodes_filter_name_area);
            lv_group_focus_obj(objects.nodes_filter_name_area);
            break;
        case 6:
            THIS->showKeyboard(objects.nodes_hl_name_area);
            lv_group_focus_obj(objects.nodes_hl_name_area);
            break;
        case 7:
            THIS->showKeyboard(objects.settings_screen_lock_password_textarea);
            lv_group_focus_obj(objects.settings_screen_lock_password_textarea);
            break;
        case 8:
            THIS->showKeyboard(objects.settings_wifi_ssid_textarea);
            lv_group_focus_obj(objects.settings_wifi_ssid_textarea);
            break;
        case 9:
            THIS->showKeyboard(objects.settings_wifi_password_textarea);
            lv_group_focus_obj(objects.settings_wifi_password_textarea);
            break;
        case 10:
            THIS->showKeyboard(objects.setup_user_short_textarea);
            lv_group_focus_obj(objects.setup_user_short_textarea);
            break;
        case 11:
            THIS->showKeyboard(objects.setup_user_long_textarea);
            lv_group_focus_obj(objects.setup_user_long_textarea);
            break;
        default:
            ILOG_ERROR("missing keyboard <-> textarea assignment");
        }
        lv_obj_has_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN) ? lv_obj_remove_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN)
                                                              : lv_obj_add_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * handle events for virtual keyboard
 */
void TFTView_320x240::ui_event_Keyboard(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_obj_t *kb = lv_event_get_target_obj(e);
        uint32_t btn_id = lv_keyboard_get_selected_button(kb);

        switch (btn_id) {
        case 22: { // enter (filtered out by one-liner text input area, so we replace it)
            // lv_obj_t *ta = lv_keyboard_get_textarea(kb);
            // lv_textarea_add_char(ta, ' ');
            // lv_textarea_add_char(ta, CR_REPLACEMENT);
            break;
        }
        case 35: { // keyboard
            lv_keyboard_set_popovers(objects.keyboard, !lv_keyboard_get_popovers(kb));
            break;
        }
        case 36: { // left
            break;
        }
        case 38: { // right
            break;
        }
        case 39: { // checkmark
            if (THIS->activePanel == objects.messages_panel) {
                THIS->hideKeyboard(objects.messages_panel);
            } else {
                lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
            }
            lv_group_focus_obj(objects.message_input_area);
            break;
        }
        default:
            break;
            // const char *txt = lv_keyboard_get_button_text(kb, btn_id);
        }
    }
}

void TFTView_320x240::ui_event_message_ready(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_READY) {
        char *txt = (char *)lv_textarea_get_text(objects.message_input_area);
        uint32_t len = strlen(txt);
        if (len) {
            if (txt[len - 1] == ' ') { // use space+return combo to start new line in same message
                lv_textarea_add_char(objects.message_input_area, CR_REPLACEMENT);
            } else {
                THIS->handleAddMessage(txt);
                lv_textarea_set_text(objects.message_input_area, "");
                if (!lv_obj_has_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN)) {
                    THIS->hideKeyboard(objects.messages_panel);
                }
                lv_group_focus_obj(objects.message_input_area);
            }
        }
    }
}

// basic settings buttons

void TFTView_320x240::ui_event_user_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_textarea_set_text(objects.settings_user_short_textarea, THIS->db.short_name);
        lv_textarea_set_text(objects.settings_user_long_textarea, THIS->db.long_name);
        lv_obj_clear_flag(objects.settings_username_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_user_short_textarea);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eUsername;
    }
}

void TFTView_320x240::ui_event_role_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone && THIS->db.config.has_device) {
        lv_dropdown_set_selected(objects.settings_device_role_dropdown, THIS->role2val(THIS->db.config.device.role));
        lv_obj_clear_flag(objects.settings_device_role_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_device_role_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eDeviceRole;
    }
}

void TFTView_320x240::ui_event_region_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone && THIS->db.config.has_lora) {
        lv_dropdown_set_selected(objects.settings_region_dropdown, THIS->db.config.lora.region - 1);
        lv_obj_clear_flag(objects.settings_region_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_region_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eRegion;
    }
}

void TFTView_320x240::ui_event_preset_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone && THIS->db.config.lora.use_preset) {
        THIS->activeSettings = eModemPreset;
        lv_dropdown_set_selected(objects.settings_modem_preset_dropdown, THIS->preset2val(THIS->db.config.lora.modem_preset));

        char buf[60];
        sprintf(buf, _("FrequencySlot: %d (%g MHz)"), THIS->db.config.lora.channel_num,
                LoRaPresets::getRadioFreq(THIS->db.config.lora.region, THIS->db.config.lora.modem_preset,
                                          THIS->db.config.lora.channel_num));
        lv_label_set_text(objects.frequency_slot_label, buf);

        uint32_t numChannels = LoRaPresets::getNumChannels(THIS->db.config.lora.region, THIS->db.config.lora.modem_preset);
        lv_slider_set_range(objects.frequency_slot_slider, 1, numChannels);
        lv_slider_set_value(objects.frequency_slot_slider, THIS->db.config.lora.channel_num, LV_ANIM_OFF);

        lv_obj_clear_flag(objects.settings_modem_preset_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_modem_preset_dropdown);
        THIS->disablePanel(objects.controller_panel);
    }
}

void TFTView_320x240::ui_event_wifi_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->db.config.has_network && THIS->activeSettings == eNone) {
        lv_textarea_set_text(objects.settings_wifi_ssid_textarea, THIS->db.config.network.wifi_ssid);
        lv_textarea_set_text(objects.settings_wifi_password_textarea, THIS->db.config.network.wifi_psk);
        lv_obj_clear_flag(objects.settings_wifi_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_wifi_ssid_textarea);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eWifi;
    }
}

void TFTView_320x240::ui_event_language_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_dropdown_set_selected(objects.settings_language_dropdown, THIS->language2val(THIS->db.uiConfig.language));
        lv_obj_clear_flag(objects.settings_language_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_language_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eLanguage;
    }
}

void TFTView_320x240::ui_event_channel_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        // primary channel is not necessarily channel[0], setup ui with primary on top
        int pos = 1;
        for (int i = 0; i < c_max_channels; i++) {
            meshtastic_Channel &ch = THIS->db.channel[i];
            if (ch.has_settings && ch.role != meshtastic_Channel_Role_DISABLED) {
                const char *channelName = ch.settings.name;
                if (ch.settings.name[0] == '\0' && ch.settings.psk.size == 1 && ch.settings.psk.bytes[0] == 0x01) {
                    channelName = LoRaPresets::modemPresetToString(THIS->db.config.lora.modem_preset);
                }
                if (ch.role == meshtastic_Channel_Role_PRIMARY) {
                    THIS->ch_label[0]->user_data = (void *)i;
                    lv_label_set_text(THIS->ch_label[0], channelName);
                } else {
                    THIS->ch_label[pos]->user_data = (void *)i;
                    lv_label_set_text(THIS->ch_label[pos++], channelName);
                }
            }
        }
        for (int i = pos; i < c_max_channels; i++) {
            THIS->ch_label[i]->user_data = (void *)-1;
            lv_label_set_text(THIS->ch_label[i], _("<unset>"));
        }
        lv_obj_clear_flag(objects.settings_channel_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_channel0_button);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eChannel;

        // create scratch channels to store temporary changes until cancelled or applied
        THIS->channel_scratch = new meshtastic_Channel[c_max_channels];
        for (int i = 0; i < c_max_channels; i++) {
            THIS->channel_scratch[i] = THIS->db.channel[i];
        }
    }
}

void TFTView_320x240::ui_event_brightness_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        char buf[20];
        uint32_t brightness = round(THIS->db.uiConfig.screen_brightness * 100.0 / 255.0);
        lv_snprintf(buf, sizeof(buf), _("Brightness: %d%%"), brightness);
        lv_label_set_text(objects.settings_brightness_label, buf);
        lv_slider_set_value(objects.brightness_slider, brightness, LV_ANIM_OFF);
        lv_obj_clear_flag(objects.settings_brightness_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.brightness_slider);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eScreenBrightness;
    }
}

void TFTView_320x240::ui_event_theme_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_dropdown_set_selected(objects.settings_theme_dropdown, THIS->db.uiConfig.theme);
        lv_obj_clear_flag(objects.settings_theme_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_theme_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eTheme;
    }
}

void TFTView_320x240::ui_event_calibration_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_screen_load_anim(objects.calibration_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    }
}

void TFTView_320x240::ui_event_timeout_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        uint32_t timeout = THIS->db.uiConfig.screen_timeout;
        char buf[32];
        if (timeout == 0)
            lv_snprintf(buf, sizeof(buf), _("Timeout: off"));
        else
            lv_snprintf(buf, sizeof(buf), _("Timeout: %ds"), timeout);
        lv_label_set_text(objects.settings_screen_timeout_label, buf);
        lv_obj_clear_flag(objects.settings_screen_timeout_panel, LV_OBJ_FLAG_HIDDEN);
        lv_slider_set_value(objects.screen_timeout_slider, timeout, LV_ANIM_OFF);
        lv_group_focus_obj(objects.screen_timeout_slider);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eScreenTimeout;
    }
}

void TFTView_320x240::ui_event_screen_lock_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        char buf[10];
        lv_snprintf(buf, 7, "%06d", THIS->db.uiConfig.pin_code);
        lv_textarea_set_text(objects.settings_screen_lock_password_textarea, buf);
        if (THIS->db.uiConfig.screen_lock) {
            lv_obj_add_state(objects.settings_screen_lock_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(objects.settings_screen_lock_switch, LV_STATE_CHECKED);
        }
        if (THIS->db.uiConfig.settings_lock) {
            lv_obj_add_state(objects.settings_settings_lock_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(objects.settings_settings_lock_switch, LV_STATE_CHECKED);
        }

        lv_obj_clear_flag(objects.settings_screen_lock_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_screen_lock_switch);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eScreenLock;
    }
}

void TFTView_320x240::ui_event_input_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        std::vector<std::string> ptr_events = THIS->inputdriver->getPointerDevices();
        std::string ptr_dropdown = _("none");
        for (std::string &s : ptr_events) {
            ptr_dropdown += '\n' + s;
        }
        lv_dropdown_set_options(objects.settings_mouse_input_dropdown, ptr_dropdown.c_str());
        std::string current_ptr = THIS->inputdriver->getCurrentPointerDevice();
        uint32_t ptrOption = lv_dropdown_get_option_index(objects.settings_mouse_input_dropdown, current_ptr.c_str());
        lv_dropdown_set_selected(objects.settings_mouse_input_dropdown, ptrOption);

        std::vector<std::string> kbd_events = THIS->inputdriver->getKeyboardDevices();
        std::string kbd_dropdown = _("none");
        for (std::string &s : kbd_events) {
            kbd_dropdown += '\n' + s;
        }
        lv_dropdown_set_options(objects.settings_keyboard_input_dropdown, kbd_dropdown.c_str());
        std::string current_kbd = THIS->inputdriver->getCurrentKeyboardDevice();
        uint32_t kbdOption = lv_dropdown_get_option_index(objects.settings_keyboard_input_dropdown, current_kbd.c_str());
        lv_dropdown_set_selected(objects.settings_keyboard_input_dropdown, kbdOption);

        lv_dropdown_get_selected_str(objects.settings_keyboard_input_dropdown, THIS->old_val1_scratch, sizeof(old_val1_scratch));
        lv_dropdown_get_selected_str(objects.settings_mouse_input_dropdown, THIS->old_val2_scratch, sizeof(old_val2_scratch));

        lv_obj_clear_flag(objects.settings_input_control_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_mouse_input_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eInputControl;
    }
}

void TFTView_320x240::ui_event_alert_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone && THIS->db.module_config.has_external_notification) {
        bool alert_enabled = THIS->db.module_config.external_notification.alert_message_buzzer &&
                             THIS->db.module_config.external_notification.enabled && !THIS->db.silent;
        if (alert_enabled) {
            lv_obj_add_state(objects.settings_alert_buzzer_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(objects.settings_alert_buzzer_switch, LV_STATE_CHECKED);
        }
        // populate dropdown
        if (lv_dropdown_get_option_count(objects.settings_ringtone_dropdown) <= 1) {
            for (int i = 2; i < numRingtones; i++) {
                lv_dropdown_add_option(objects.settings_ringtone_dropdown, ringtone[i].name, i);
            }
        }

        lv_dropdown_set_selected(objects.settings_ringtone_dropdown, THIS->db.uiConfig.ring_tone_id - 1);
        lv_obj_clear_flag(objects.settings_alert_buzzer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_alert_buzzer_switch);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eAlertBuzzer;
    }
}

// backup & restore
void TFTView_320x240::ui_event_backup_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_obj_clear_flag(objects.settings_backup_restore_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_backup_restore_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eBackupRestore;
    }
}

// configuration reset
void TFTView_320x240::ui_event_reset_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_obj_clear_flag(objects.settings_reset_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_reset_dropdown);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eReset;
    }
}

// reboot / shutdown
void TFTView_320x240::ui_event_reboot_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eNone) {
        lv_obj_remove_flag(objects.boot_logo_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.bluetooth_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.boot_logo_arc, LV_OBJ_FLAG_HIDDEN);
        lv_screen_load_anim(objects.boot_screen, LV_SCR_LOAD_ANIM_FADE_IN, 1000, 0, false);
        lv_obj_clear_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.cancel_reboot_button);
        THIS->disablePanel(objects.controller_panel);
        THIS->disablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eReboot;
    }
}

void TFTView_320x240::ui_event_device_reboot_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        THIS->controller->requestReboot(5, THIS->ownNode);
        lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 4000, 1000, false);
        lv_obj_add_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
        if (THIS->controller->isStandalone()) {
            lv_timer_create(timer_event_reboot, 4000, NULL);
        }
    }
}

void TFTView_320x240::ui_event_device_progmode_button(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }

        meshtastic_Config_NetworkConfig &network = THIS->db.config.network;
        if (network.wifi_enabled) {
            network.wifi_enabled = false;
            THIS->controller->sendConfig(meshtastic_Config_NetworkConfig{network});
        }
        meshtastic_Config_BluetoothConfig &bluetooth = THIS->db.config.bluetooth;
        bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
        bluetooth.fixed_pin = random(100000, 999999);
        bluetooth.enabled = true;
        THIS->controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, THIS->ownNode);
        lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 4000, 1000, false);
        lv_obj_add_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        if (!THIS->controller->isStandalone()) {
#if defined(HAS_SCREEN) && HAS_SCREEN == 1
            ignoreClicked = true;
            // open dialog
            lv_obj_remove_flag(objects.settings_reboot_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.settings_reboot_panel);
            THIS->activeSettings = eDisplayMode;
#endif
        }
    }
}

void TFTView_320x240::ui_event_device_shutdown_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        THIS->controller->requestShutdown(5, THIS->ownNode);
        lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 4000, 1000, false);
        lv_obj_add_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
        if (THIS->controller->isStandalone()) {
            lv_timer_create(timer_event_shutdown, 4000, NULL);
        }
    }
}

void TFTView_320x240::ui_event_device_cancel_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        lv_obj_add_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.settings_reboot_panel, LV_OBJ_FLAG_HIDDEN);
        THIS->enablePanel(objects.controller_panel);
        THIS->enablePanel(objects.tab_page_basic_settings);
        lv_group_focus_obj(objects.basic_settings_reboot_button);
        THIS->activeSettings = eNone;
    }
}

void TFTView_320x240::ui_event_modify_channel(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && THIS->activeSettings == eChannel) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        uint32_t btn_id = (unsigned long)e->user_data;
        int8_t ch = (signed long)THIS->ch_label[btn_id]->user_data;
        if (ch != -1) {
            meshtastic_ChannelSettings_psk_t &psk = THIS->channel_scratch[ch].settings.psk;
            std::string base64 = THIS->pskToBase64(psk.bytes, psk.size);
            lv_textarea_set_text(objects.settings_modify_channel_psk_textarea, base64.c_str());
            lv_textarea_set_text(objects.settings_modify_channel_name_textarea, THIS->channel_scratch[ch].settings.name);
            objects.settings_modify_channel_name_textarea->user_data = (void *)btn_id;
        } else {
            for (int i = 0; i < c_max_channels; i++) {
                if (THIS->channel_scratch[i].role == meshtastic_Channel_Role_DISABLED) {
                    // the first created channel is PRIMARY
                    bool found = false;
                    for (int j = 0; j < c_max_channels; j++) {
                        if (THIS->channel_scratch[j].role == meshtastic_Channel_Role_PRIMARY) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        THIS->channel_scratch[i].role = meshtastic_Channel_Role_PRIMARY;
                        if (i == 0) {
                            btn_id = 0; // place on top
                        } else {
                            // FIXME: swap ids as in long press
                            ILOG_ERROR("node does not have primary channel!");
                        }
                    } else
                        THIS->channel_scratch[i].role = meshtastic_Channel_Role_SECONDARY;

                    lv_textarea_set_text(objects.settings_modify_channel_psk_textarea, "");
                    lv_textarea_set_text(objects.settings_modify_channel_name_textarea, "");
                    THIS->ch_label[btn_id]->user_data = (void *)i;
                    objects.settings_modify_channel_name_textarea->user_data = (void *)btn_id;
                    break;
                }
            }
        }

        THIS->disablePanel(objects.settings_channel_panel);
        lv_obj_clear_flag(objects.settings_modify_channel_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.settings_modify_channel_name_textarea);
        THIS->activeSettings = eModifyChannel;
    }
#if 0 // TODO: simple swap not allowed: primary channel must be id 0
    else if (event_code == LV_EVENT_LONG_PRESSED && THIS->activeSettings == eChannel) {
        ignoreClicked = true;
        // make channel primary on long press; swap with current primary (role, id and name)
        uint8_t btn_id = (uint8_t)(unsigned long)e->user_data;
        int8_t ch = (signed long)THIS->ch_label[btn_id]->user_data;
        if (btn_id != 0 && ch != -1) {
            int32_t primary_id = (signed long)THIS->ch_label[0]->user_data;
            THIS->channel_scratch[primary_id].role = meshtastic_Channel_Role_SECONDARY;
            THIS->channel_scratch[ch].role = meshtastic_Channel_Role_PRIMARY;
            THIS->ch_label[0]->user_data = (void *)(uint32_t)ch;
            THIS->ch_label[btn_id]->user_data = (void *)primary_id;
            lv_label_set_text(THIS->ch_label[0], THIS->channel_scratch[ch].settings.name);
            lv_label_set_text(THIS->ch_label[btn_id], THIS->channel_scratch[primary_id].settings.name);
        }
    }
#endif
}

void TFTView_320x240::ui_event_generate_psk(lv_event_t *e)
{
    std::string base64 = lv_textarea_get_text(objects.settings_modify_channel_psk_textarea);
    if (base64.size() == 0 || THIS->qr) {
        meshtastic_ChannelSettings_psk_t psk{.size = 32};
        std::mt19937 generator(millis() + psk.bytes[7]); // Mersenne Twister number generator
        for (int i = 0; i < 8; i++) {
            int r = generator();
            memcpy(&psk.bytes[i * 4], &r, 4);
        }
        base64 = THIS->pskToBase64(psk.bytes, psk.size);
        lv_textarea_set_text(objects.settings_modify_channel_psk_textarea, base64.c_str());
    }

    std::string base64Https = base64;
    for (char &c : base64Https) {
        if (c == '+')
            c = '-';
        else if (c == '/')
            c = '_';
        else if (c == '=')
            c = '\0'; // remove paddings at the end of the url
    }
    std::string qr = "https://meshtastic.org/e/#" + base64Https;
    lv_obj_remove_flag(objects.settings_modify_channel_qr_panel, LV_OBJ_FLAG_HIDDEN);
    THIS->qr = THIS->showQrCode(objects.settings_modify_channel_qr_panel, qr.c_str());
    lv_obj_add_state(objects.keyboard_button_3, LV_STATE_DISABLED);
    lv_obj_add_state(objects.keyboard_button_4, LV_STATE_DISABLED);
}

void TFTView_320x240::ui_event_qr_code(lv_event_t *e)
{
    lv_obj_remove_state(objects.keyboard_button_3, LV_STATE_DISABLED);
    lv_obj_remove_state(objects.keyboard_button_4, LV_STATE_DISABLED);
    lv_obj_add_flag(objects.settings_modify_channel_qr_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete(THIS->qr);
    THIS->qr = nullptr;
}

void TFTView_320x240::ui_event_delete_channel(lv_event_t *e)
{
    lv_textarea_set_text(objects.settings_modify_channel_psk_textarea, "");
    lv_textarea_set_text(objects.settings_modify_channel_name_textarea, "");
}

void TFTView_320x240::ui_event_calibration_screen_loaded(lv_event_t *e)
{
    uint16_t *parameters = (uint16_t *)THIS->db.uiConfig.calibration_data.bytes;
    memset(parameters, 0, 16); // clear all calibration data
    bool done = THIS->displaydriver->calibrate(parameters);
    THIS->db.uiConfig.calibration_data.size = 16;
    char buf[32];
    lv_snprintf(buf, sizeof(buf), _("Screen Calibration: %s"), done ? _("done") : _("default"));
    lv_label_set_text(objects.basic_settings_calibration_label, buf);
    lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, false);
    THIS->controller->storeUIConfig(THIS->db.uiConfig);
}

void TFTView_320x240::ui_event_pin_screen_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED && lv_scr_act() == objects.lock_screen) {
        static const char *hidden[7] = {"o o o o o o", "* o o o o o", "* * o o o o", "* * * o o o",
                                        "* * * * o o", "* * * * * o", "* * * * * *"};
        static char pinEntered[7]{};
        lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
        uint32_t id = lv_buttonmatrix_get_selected_button(obj);
        const char *key = lv_buttonmatrix_get_button_text(obj, id);
        switch (*key) {
        case 'X': {
            pinKeys = 0;
            lv_label_set_text(objects.lock_screen_digits_label, hidden[pinKeys]);
            if (!screenLocked) {
                lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_FADE_IN, 100, 0, false);
            } else {
                // TODO: init screen saver
            }
            break;
        }
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            if (pinKeys < 6) {
                pinEntered[pinKeys++] = *key;
                lv_label_set_text(objects.lock_screen_digits_label, hidden[pinKeys]);

                char buf[10];
                lv_snprintf(buf, 7, "%06d", THIS->db.uiConfig.pin_code);
                if (pinKeys == 6 && strcmp(pinEntered, buf) == 0) {
                    // unlock screen
                    pinKeys = 0;
                    screenLocked = false;
                    lv_obj_clear_flag(objects.tab_page_basic_settings, LV_OBJ_FLAG_HIDDEN);
                    lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_FADE_IN, 100, 0, false);
                    lv_label_set_text(objects.lock_screen_digits_label, hidden[pinKeys]);
                }
            }
            break;
        }
        case 'D': {
            if (pinKeys > 0) {
                pinEntered[--pinKeys] = '\0';
                lv_label_set_text(objects.lock_screen_digits_label, hidden[pinKeys]);
            }
            break;
        }
        default:
            break;
        }
    }
}

void TFTView_320x240::ui_event_backup_restore_radio_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        lv_obj_remove_state(objects.settings_backup_checkbox, LV_STATE_CHECKED);
        lv_obj_remove_state(objects.settings_restore_checkbox, LV_STATE_CHECKED);
        lv_obj_add_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    }
}

void TFTView_320x240::ui_event_zoomSlider(lv_event_t *e)
{
    THIS->map->setZoom(lv_slider_get_value(objects.zoom_slider));
    THIS->updateLocationMap(THIS->map->getObjectsOnMap());
}

void TFTView_320x240::ui_event_zoomIn(lv_event_t *e)
{
    THIS->map->setZoom(MapTileSettings::getZoomLevel() + 1);
    THIS->updateLocationMap(THIS->map->getObjectsOnMap());
}

void TFTView_320x240::ui_event_zoomOut(lv_event_t *e)
{
    THIS->map->setZoom(MapTileSettings::getZoomLevel() - 1);
    THIS->updateLocationMap(THIS->map->getObjectsOnMap());
}

void TFTView_320x240::ui_event_lockGps(lv_event_t *e)
{
    bool gpsLocked = lv_obj_has_state(objects.gps_lock_button, LV_STATE_CHECKED);
    THIS->map->setLocked(gpsLocked);
    THIS->db.uiConfig.map_data.follow_gps = gpsLocked;
    THIS->controller->storeUIConfig(THIS->db.uiConfig);
}

void TFTView_320x240::ui_event_mapBrightnessSlider(lv_event_t *e)
{
    uint32_t br = lv_slider_get_value(objects.map_brightness_slider);
    lv_obj_set_style_bg_color(objects.map_panel, lv_color_make(br, br, br), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(objects.raw_map_panel, lv_color_make(br, br, br), LV_PART_MAIN | LV_STATE_DEFAULT);
}

void TFTView_320x240::ui_event_mapContrastSlider(lv_event_t *e)
{
    uint32_t ct = lv_slider_get_value(objects.map_contrast_slider);
    lv_obj_set_style_opa(objects.raw_map_panel, ct, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void TFTView_320x240::ui_event_map_style_dropdown(lv_event_t *e)
{
    lv_dropdown_get_selected_str(objects.map_style_dropdown, THIS->db.uiConfig.map_data.style,
                                 sizeof(THIS->db.uiConfig.map_data.style));
    MapTileSettings::setTileStyle(THIS->db.uiConfig.map_data.style);
    // set url provider if exist
    std::string url = sdCard->getUrlProvider(MapTileSettings::getPrefix(), THIS->db.uiConfig.map_data.style);
    if (!url.empty()) {
        std::string provider = std::string("URL: ") + THIS->db.uiConfig.map_data.style;
        int entry = TileProvider::addTemplate(provider, url);
        lv_dropdown_set_selected(objects.map_url_dropdown, entry);
        TileProvider::selectTemplate(entry);
    }
    MapTileSettings::setSaveOK(!url.empty()); // enable SD save if .url exists

    THIS->controller->storeUIConfig(THIS->db.uiConfig);
    lv_obj_add_flag(objects.map_osd_panel, LV_OBJ_FLAG_HIDDEN);
    THIS->map->forceRedraw();
}

void TFTView_320x240::ui_event_map_url_dropdown(lv_event_t *e)
{
    uint32_t urlId = lv_dropdown_get_selected(objects.map_url_dropdown);
    TileProvider::selectTemplate(urlId);
    MapTileSettings::setSaveOK(false);
    lv_obj_add_flag(objects.map_osd_panel, LV_OBJ_FLAG_HIDDEN);
    THIS->map->forceRedraw();
}

void TFTView_320x240::ui_event_mapNodeButton(lv_event_t *e)
{
    // navigate to node in node list
    uint32_t nodeNum = (unsigned long)e->user_data;
    ILOG_DEBUG("map node %08x", nodeNum);
    lv_obj_t *panel = THIS->nodes[nodeNum];
    THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
    lv_obj_scroll_to_view(panel, LV_ANIM_ON);
    if (panel != currentPanel)
        ui_event_NodeButton(e);
}

void TFTView_320x240::ui_event_chatNodeButton(lv_event_t *e)
{
    uint32_t nodeNum = (unsigned long)e->user_data;
    auto it = THIS->nodes.find(nodeNum);
    if (it != THIS->nodes.end()) {
        lv_obj_t *panel = it->second;
        THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
        lv_obj_scroll_to_view(panel, LV_ANIM_ON);
        if (panel != currentPanel)
            ui_event_NodeButton(e);
    }
}

void TFTView_320x240::ui_event_positionButton(lv_event_t *e)
{
    // navigate to position in map
    lv_obj_t *p = (lv_obj_t *)e->user_data;
    int32_t lat = (long)p->LV_OBJ_IDX(node_pos1_idx)->user_data;
    int32_t lon = (long)p->LV_OBJ_IDX(node_pos2_idx)->user_data;
    if (lat && lon) {
        THIS->ui_set_active(objects.map_button, objects.map_panel, objects.top_map_panel);
        if (!THIS->map) {
            THIS->loadMap();
        }
        THIS->map->setScrolledPosition(lat * 1e-7, lon * 1e-7);
    }
}

void TFTView_320x240::ui_screen_event_cb(lv_event_t *e)
{
    if (THIS->activePanel == objects.map_panel) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        switch (dir) {
        case LV_DIR_LEFT:
            e->user_data = (void *)6;
            break;
        case LV_DIR_RIGHT:
            e->user_data = (void *)4;
            break;
        case LV_DIR_TOP:
            e->user_data = (void *)2;
            break;
        case LV_DIR_BOTTOM:
            e->user_data = (void *)8;
            break;
        default:
            break;
        }
        ILOG_DEBUG("gesture: %d", (uint16_t)dir);
        THIS->ui_event_arrow(e);
    }
}

void TFTView_320x240::ui_event_arrow(lv_event_t *e)
{
    if (THIS->map && THIS->map->redrawComplete()) {
        uint16_t deltaX = 0;
        uint16_t deltaY = 0;
        ScrollDirection direction = (ScrollDirection)(unsigned long)e->user_data;
        switch (direction) {
        case scrollDownLeft:
            deltaX = 1;
            deltaY = -1;
            break;
        case scrollDown:
            deltaX = 0;
            deltaY = -1;
            break;
        case scrollDownRight:
            deltaX = -1;
            deltaY = -1;
            break;
        case scrollLeft:
            deltaX = 1;
            deltaY = 0;
            break;
        case scrollRight:
            deltaX = -1;
            deltaY = 0;
            break;
        case scrollUpLeft:
            deltaX = 1;
            deltaY = 1;
            break;
        case scrollUp:
            deltaX = 0;
            deltaY = 1;
            break;
        case scrollUpRight:
            deltaX = -1;
            deltaY = 1;
            break;
        default:
            break;
        };
        if (!THIS->map->scroll(deltaX, deltaY))
            THIS->map->forceRedraw();
    }
    THIS->updateLocationMap(THIS->map->getObjectsOnMap());
}

void TFTView_320x240::ui_event_navHome(lv_event_t *e)
{
    static bool ignoreClicked = false;
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        if (ignoreClicked) { // prevent long press to enter this setting
            ignoreClicked = false;
            return;
        }
        THIS->map->moveHome();
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        ignoreClicked = true;
        float lat, lon;
        THIS->map->setHomePosition();
        THIS->map->getHomeLocation(lat, lon);

        int32_t ilat = lat * 1e7f;
        int32_t ilon = lon * 1e7f;
        THIS->db.uiConfig.has_map_data = true;
        THIS->db.uiConfig.map_data.has_home = true;
        THIS->db.uiConfig.map_data.home.latitude = ilat;
        THIS->db.uiConfig.map_data.home.longitude = ilon;
        THIS->db.uiConfig.map_data.home.zoom = MapTileSettings::getZoomLevel();
        THIS->controller->storeUIConfig(THIS->db.uiConfig);

        meshtastic_Config_PositionConfig &position = THIS->db.config.position;
        if (position.fixed_position) {
            THIS->updatePosition(THIS->ownNode, ilat, ilon, 0, 0, 0);
            if (position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
                // grey out text to indicate it's a fixed position vs. actual GPS position
                Themes::recolorText(objects.home_location_label, false);
                THIS->controller->sendConfig(meshtastic_Position{.latitude_i = ilat,
                                                                 .longitude_i = ilon,
                                                                 .time = uint32_t(VALID_TIME(THIS->actTime) ? THIS->actTime : 0),
                                                                 .location_source = meshtastic_Position_LocSource_LOC_MANUAL});
            }
        }
    }
}

void TFTView_320x240::loadMap(void)
{
    if (!map) {
        // the tile service must exist only ONCE (its ctor registers an LVGL fs driver),
        // and is shared with the Maps app's own MapPanel — see sharedTileService()
#if LV_USE_FS_ARDUINO_SD
        map = new MapPanel(objects.raw_map_panel);
#elif defined(HAS_SD_MMC) || defined(HAS_SDCARD)
        ITileService *tileService = sharedTileService();
        map = new MapPanel(objects.raw_map_panel, tileService);
        map->setBackupService(
            new URLService([tileService](const char *name, void *img, size_t len) { return tileService->save(name, img, len); }));
#elif defined(ARCH_PORTDUINO)
        map = new MapPanel(objects.raw_map_panel, sharedTileService()); // TODO: LinuxFileSystemService
#else
        map = new MapPanel(objects.raw_map_panel, sharedTileService());
#endif
        map->setHomeLocationImage(objects.home_location_image);
        lv_obj_add_flag(objects.home_location_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(objects.home_location_image, ui_event_mapNodeButton, LV_EVENT_CLICKED, (void *)ownNode);

        // Leaving the Meshtastic screen (to the launcher or one of our apps)? Drop the
        // mesh map's tiles to free RAM; the pump rebuilds them when it's back on screen.
        lv_obj_add_event_cb(
            objects.main_screen,
            [](lv_event_t *) {
                if (THIS->map)
                    THIS->map->releaseTiles();
            },
            LV_EVENT_SCREEN_UNLOADED, NULL);

        // center map to GPS > home > other nodes > default location
        if (db.config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
            map->setGpsPositionImage(objects.gps_position_image);
            lv_obj_clear_flag(objects.gps_position_image, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(objects.gps_position_image, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.gps_lock_button, LV_OBJ_FLAG_HIDDEN);
        }
        if (hasPosition) {
            if (db.uiConfig.map_data.has_home) {
                map->setHomeLocation(db.uiConfig.map_data.home.latitude * 1e-7, db.uiConfig.map_data.home.longitude * 1e-7);
                map->setZoom(db.uiConfig.map_data.home.zoom);
            } else {
                map->setHomeLocation(myLatitude * 1e-7, myLongitude * 1e-7);
                map->setZoom(13);
            }
            map->setGpsPosition(myLatitude * 1e-7, myLongitude * 1e-7);
        } else if (db.uiConfig.map_data.has_home) {
            map->setHomeLocation(db.uiConfig.map_data.home.latitude * 1e-7, db.uiConfig.map_data.home.longitude * 1e-7);
            map->setZoom(db.uiConfig.map_data.home.zoom);
        } else if (nodeObjects.size() >= 1) {
            // no gps, no saved position then center the home location among other available nodes
            std::vector<int32_t> sortedLat;
            std::vector<int32_t> sortedLon;
            sortedLat.reserve(nodeObjects.size());
            sortedLon.reserve(nodeObjects.size());
            for (auto it : nodeObjects) {
                lv_obj_t *p = nodes[it.first];
                int32_t lat = (long)p->LV_OBJ_IDX(node_pos1_idx)->user_data;
                int32_t lon = (long)p->LV_OBJ_IDX(node_pos2_idx)->user_data;
                if (lat && lon) {
                    sortedLat.push_back(lat);
                    sortedLon.push_back(lon);
                }
            }
            std::sort(sortedLat.begin(), sortedLat.end());
            std::sort(sortedLon.begin(), sortedLon.end());
            int64_t latcenter = 0;
            int64_t loncenter = 0;
            int32_t count = 0;
            // select just the closest 60% of nodes, ignore the rest
            int pp = 100 / 20;
            for (int i = sortedLat.size() / pp; i < pp * sortedLat.size() / pp; i++) {
                latcenter += sortedLat[i];
                loncenter += sortedLon[i];
                count++;
            }
            latcenter /= count;
            loncenter /= count;
            map->setHomeLocation(latcenter * 1e-7, loncenter * 1e-7);

            // calculate optimal zoom factor to fit in all nodes of this range
            lv_obj_update_layout(objects.raw_map_panel);
            float rangeDeg = 1e-7 * (sortedLon[(pp - 1) * sortedLon.size() / pp] - sortedLon[sortedLon.size() / pp]);
            float distanceKm = abs(rangeDeg * 111.32 * cos(1e-7 * sortedLat[sortedLat.size() / 2]));
            uint32_t zoom = sqrt(156.543034f / distanceKm * abs(cos(1e-7 * sortedLat[sortedLat.size() / 2])) * 256) + 1;
            map->setZoom(zoom);
        } else {
            // use default location @theBigBentern
            map->setZoom(3);
        }

        if (db.uiConfig.map_data.follow_gps) {
            lv_obj_set_state(objects.gps_lock_button, LV_STATE_CHECKED, true);
            map->setLocked(true);
        }

        // finally add all node images to the map
        if (!nodeObjects.empty()) {
            for (auto it : nodeObjects) {
                lv_obj_t *p = nodes[it.first];
                float lat = 1e-7 * (long)p->LV_OBJ_IDX(node_pos1_idx)->user_data;
                float lon = 1e-7 * (long)p->LV_OBJ_IDX(node_pos2_idx)->user_data;
                map->add(it.first, lat, lon, drawObjectCB);
                lv_obj_add_flag(it.second, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(it.second, ui_event_mapNodeButton, LV_EVENT_CLICKED, (void *)it.first);
            }
        }
        updateLocationMap(map->getObjectsOnMap());
    }

    if (sdCard) {
        if (!sdCard->isUpdated()) {
            map->setNoTileImage(&img_no_tile_image);
            std::set<std::string> mapStyles = sdCard->loadMapStyles(MapTileSettings::getPrefix());
            if (mapStyles.find("/map") != mapStyles.end()) {
                // no styles found, but the /map directory, so use it
                MapTileSettings::setPrefix("/map");
                MapTileSettings::setTileStyle("");
                lv_obj_add_flag(objects.map_style_dropdown, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(objects.map_url_dropdown, LV_OBJ_FLAG_HIDDEN);
            } else if (!mapStyles.empty()) {
                // populate style dropdown
                bool savedStyleOK = false;
                lv_dropdown_clear_options(objects.map_style_dropdown);
                for (auto it : mapStyles) {
                    // add url provider if exist
                    int urlEntry = -1;
                    std::string url = sdCard->getUrlProvider(MapTileSettings::getPrefix(), it.c_str());
                    if (!url.empty()) {
                        urlEntry = TileProvider::addTemplate("URL: " + it, url);
                        lv_dropdown_add_option(objects.map_url_dropdown, std::string("URL: " + it).c_str(), LV_DROPDOWN_POS_LAST);
                    }
                    lv_dropdown_add_option(objects.map_style_dropdown, it.c_str(), LV_DROPDOWN_POS_LAST);
                    if (it == db.uiConfig.map_data.style) {
                        lv_dropdown_set_selected(objects.map_style_dropdown, LV_DROPDOWN_POS_LAST);
                        MapTileSettings::setTileStyle(db.uiConfig.map_data.style);
                        savedStyleOK = true;
                        if (urlEntry >= 0) {
                            // set provider url to current style
                            ILOG_DEBUG("set provider url to %s", url.c_str());
                            TileProvider::selectTemplate(urlEntry);
                        }
                    }
                }
                lv_dropdown_set_options(objects.map_url_dropdown, TileProvider::providers().c_str());
                lv_dropdown_set_selected(objects.map_url_dropdown, TileProvider::selectedTemplate());

                if (!savedStyleOK) {
                    // no such style on SD, pick first one we found
                    char style[30];
                    lv_dropdown_set_selected(objects.map_style_dropdown, 0);
                    lv_dropdown_get_selected_str(objects.map_style_dropdown, style, sizeof(style));
                    MapTileSettings::setTileStyle(style);
                }

                MapTileSettings::setSaveOK(savedStyleOK); // allow SD save only for identical style
                MapTileSettings::setPrefix("/maps");
            } else {
                // messageAlert(_("No map tiles found on SDCard!"), true);
            }
            map->forceRedraw();
        }
    } else {
        lv_dropdown_set_options(objects.map_style_dropdown, "");
    }

    lv_obj_clear_flag(objects.map_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(objects.raw_map_panel, LV_OBJ_FLAG_HIDDEN);

    if (meshMapStale) {
        // the Maps app moved the shared zoom / redraw statics while we were away;
        // setZoom() with the current level re-centers and forces a full tile rebuild
        meshMapStale = false;
        map->setZoom(MapTileSettings::getZoomLevel());
    }
}

void TFTView_320x240::updateLocationMap(uint32_t num)
{
    lv_label_set_text_fmt(objects.top_map_label, _("Locations Map (%d/%d)"), num, nodeCount);
}

/**
 * add node location image for display on map
 */
void TFTView_320x240::addOrUpdateMap(uint32_t nodeNum, int32_t lat, int32_t lon)
{
    auto it = nodeObjects.find(nodeNum);
    if (it == nodeObjects.end()) {
        uint32_t bgColor, fgColor;
        std::tie(bgColor, fgColor) = nodeColor(nodeNum);
        lv_obj_t *img = lv_image_create(objects.raw_map_panel);
        lv_obj_set_size(img, 40, 35);
        lv_img_set_src(img, &img_circle_image);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_MID);
        lv_obj_set_style_opa(img, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor(img, lv_color_hex(bgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(img, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *lbl = lv_label_create(img);
        lv_obj_set_pos(lbl, 0, 0);
        lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_align(lbl, LV_ALIGN_BOTTOM_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_align(lbl, LV_ALIGN_BOTTOM_MID, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *p = nodes[nodeNum];
        lv_label_set_text_fmt(lbl, "%s", lv_label_get_text(p->LV_OBJ_IDX(node_lbs_idx)));

        // position label callback
        lv_obj_add_flag(p->LV_OBJ_IDX(node_pos1_idx), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(p->LV_OBJ_IDX(node_pos1_idx), ui_event_positionButton, LV_EVENT_CLICKED, (void *)p);

        nodeObjects[nodeNum] = img;
        if (map) {
            map->add(nodeNum, lat * 1e-7, lon * 1e-7, drawObjectCB);
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(img, ui_event_mapNodeButton, LV_EVENT_CLICKED, (void *)nodeNum);
            updateLocationMap(map->getObjectsOnMap());
        }
    } else {
        if (map) {
            map->update(it->first, lat * 1e-7, lon * 1e-7);
        }
    }
}

void TFTView_320x240::removeFromMap(uint32_t nodeNum)
{
    auto it = nodeObjects.find(nodeNum);
    if (it == nodeObjects.end())
        return;

    lv_obj_t *img = it->second;
    if (map) {
        map->remove(it->first);
        updateLocationMap(map->getObjectsOnMap());
    }
    nodeObjects.erase(nodeNum);
    lv_obj_remove_event_cb(img, ui_event_mapNodeButton);
    lv_obj_delete(img);
}

void TFTView_320x240::ui_event_mesh_detector(lv_event_t *e)
{
    THIS->ui_set_active(objects.settings_button, objects.mesh_detector_panel, objects.top_mesh_detector_panel);
}

void TFTView_320x240::ui_event_mesh_detector_start(_lv_event_t *e)
{
    lv_obj_add_flag(objects.detector_contact_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.detector_heard_label, LV_OBJ_FLAG_HIDDEN);
    if (!THIS->detectorRunning) {
        lv_label_set_text(objects.detector_start_label, _("Stop"));

        // create radar animation
        lv_anim_t &a = THIS->radar;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objects.radar_beam);
        lv_anim_set_values(&a, 0, 3600);
        lv_anim_set_repeat_count(&a, 1800);
        lv_anim_set_duration(&a, 7200);
        lv_anim_set_exec_cb(&a, ui_anim_radar_cb);
        lv_anim_start(&a);
        lv_obj_clear_flag(objects.detector_radar_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(objects.detector_start_label, _("Start"));
        lv_anim_del(&objects.radar_beam, ui_anim_radar_cb);
        lv_obj_add_flag(objects.detector_radar_panel, LV_OBJ_FLAG_HIDDEN);
    }
    THIS->detectorRunning = !THIS->detectorRunning;
    THIS->controller->sendPing();
}

void TFTView_320x240::ui_event_signal_scanner(lv_event_t *e)
{
    if (currentPanel) {
        THIS->setNodeImage(currentNode, (MeshtasticView::eRole)(unsigned long)currentPanel->LV_OBJ_IDX(node_img_idx)->user_data,
                           false, objects.signal_scanner_node_image);
        const char *lbs = lv_label_get_text(currentPanel->LV_OBJ_IDX(node_lbs_idx));
        lv_label_set_text(objects.signal_scanner_node_button_label, lbs);
        lv_obj_clear_state(objects.signal_scanner_start_button, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(objects.signal_scanner_node_button_label, _("choose\nnode"));
        lv_obj_add_state(objects.signal_scanner_start_button, LV_STATE_DISABLED);
    }
    lv_label_set_text(objects.signal_scanner_start_label, _("Start"));
    THIS->ui_set_active(objects.settings_button, objects.signal_scanner_panel, objects.top_signal_scanner_panel);
}

void TFTView_320x240::ui_event_signal_scanner_node(lv_event_t *e)
{
    THIS->chooseNodeSignalScanner = true;
    THIS->selectedHops = lv_dropdown_get_selected(objects.nodes_filter_hops_dropdown);
    lv_dropdown_set_selected(objects.nodes_filter_hops_dropdown, 7); // 0 hops away
    THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
    THIS->updateNodesFiltered(true);
    THIS->updateNodesStatus();
}

void TFTView_320x240::ui_event_signal_scanner_start(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (currentNode) {
        static bool ignoreClicked = false;
        if (event_code == LV_EVENT_CLICKED) {
            if (ignoreClicked) {
                ignoreClicked = false;
                return;
            }
            if (spinnerButton) {
                lv_label_set_text(objects.signal_scanner_start_label, _("Start"));
                lv_obj_delete(spinnerButton);
                spinnerButton = nullptr;
                THIS->scans = 0;
            } else {
                THIS->scanSignal(0);
            }
        } else if (event_code == LV_EVENT_LONG_PRESSED) {
            ignoreClicked = true;
            lv_obj_t *obj = lv_spinner_create(objects.signal_scanner_panel);
            spinnerButton = obj;
            spinnerButton->user_data = (void *)objects.signal_scanner_panel;
            lv_spinner_set_anim_params(obj, 5000, 300);
            lv_obj_set_pos(obj, 0, -50);
            lv_obj_set_size(obj, 68, 68);
            lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            add_style_spinner_style(obj);
            lv_label_set_text(objects.signal_scanner_start_label, "30s");
            THIS->scans = 6 + 1;
        }
    }
}

void TFTView_320x240::ui_event_trace_route(lv_event_t *e)
{
    // show still old route setup while processing is ongoing
    time_t now;
    time(&now);
    if (spinnerButton && (now - startTime) < 30) {
        THIS->ui_set_active(objects.settings_button, objects.trace_route_panel, objects.top_trace_route_panel);
        return;
    }
    THIS->removeSpinner();

    // remove old route except first button and spinner panel
    ILOG_DEBUG("removing old route: %d %d %d", lv_obj_get_child_cnt(objects.trace_route_panel),
               lv_obj_get_child_cnt(objects.route_towards_panel), lv_obj_get_child_cnt(objects.route_back_panel));

    uint16_t children = lv_obj_get_child_cnt(objects.trace_route_panel) - 1;
    while (children > 1) {
        if (objects.trace_route_panel->spec_attr->children[children]->class_p == &lv_button_class) {
            lv_obj_delete(objects.trace_route_panel->spec_attr->children[children]);
        }
        children--;
    }

    // forward route
    children = lv_obj_get_child_cnt(objects.route_towards_panel);
    while (children > 0) {
        children--;
        if (objects.route_towards_panel->spec_attr->children[children]->class_p == &lv_button_class) {
            lv_obj_delete(objects.route_towards_panel->spec_attr->children[children]);
        }
    }

    // backward route
    children = lv_obj_get_child_cnt(objects.route_back_panel);
    while (children > 0) {
        children--;
        if (objects.route_back_panel->spec_attr->children[children]->class_p == &lv_button_class) {
            lv_obj_delete(objects.route_back_panel->spec_attr->children[children]);
        }
    }

    lv_obj_clear_flag(objects.start_button_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.hop_routes_panel, LV_OBJ_FLAG_HIDDEN);

    if (currentPanel) {
        THIS->setNodeImage(THIS->currentNode,
                           (MeshtasticView::eRole)(unsigned long)currentPanel->LV_OBJ_IDX(node_img_idx)->user_data, false,
                           objects.trace_route_to_image);
        const char *lbl = lv_label_get_text(currentPanel->LV_OBJ_IDX(node_lbl_idx));
        lv_label_set_text(objects.trace_route_to_button_label, lbl);
        lv_obj_clear_state(objects.trace_route_start_button, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(objects.trace_route_to_button_label, _("choose target node"));
        lv_obj_add_state(objects.trace_route_start_button, LV_STATE_DISABLED);
    }
    lv_label_set_text(objects.trace_route_start_label, _("Start"));
    THIS->ui_set_active(objects.settings_button, objects.trace_route_panel, objects.top_trace_route_panel);
}

void TFTView_320x240::ui_event_trace_route_to(lv_event_t *e)
{
    if (!spinnerButton) {
        THIS->chooseNodeTraceRoute = true;
        THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
    }
}

void TFTView_320x240::ui_event_trace_route_start(lv_event_t *e)
{
    if (!spinnerButton) {
        if (currentPanel) {
            time(&startTime);
            lv_obj_t *obj = lv_spinner_create(objects.start_button_panel);
            spinnerButton = obj;
            lv_spinner_set_anim_params(obj, 5000, 300);
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, 68, 68);
            lv_obj_set_style_align(obj, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
            add_style_spinner_style(obj);
            lv_label_set_text(objects.trace_route_start_label, "30s");

            // retrieve nodeNum from current node
            // FIXME: remove for loop
            for (auto &it : THIS->nodes) {
                if (it.second == currentPanel) {
                    uint32_t requestId;
                    uint32_t to = it.first;
                    uint8_t ch = (uint8_t)(unsigned long)currentPanel->user_data;
                    // trial: hoplimit optimization for direct messages
                    int8_t hopsAway = (signed long)THIS->nodes[to]->LV_OBJ_IDX(node_sig_idx)->user_data;
                    if (hopsAway < 0)
                        hopsAway = 5;
                    uint8_t hopLimit = (hopsAway < THIS->db.config.lora.hop_limit ? hopsAway + 1 : hopsAway);
                    requestId = THIS->requests.addRequest(to, ResponseHandler::TraceRouteRequest);
                    THIS->controller->traceRoute(to, ch, hopLimit, requestId);
                    break;
                }
            }
        }
    } else {
        // restart
        ui_event_trace_route(e);
    }
}

void TFTView_320x240::ui_event_trace_route_node(lv_event_t *e)
{
    // navigate to node in node list
    lv_obj_t *panel = (lv_obj_t *)e->user_data;
    THIS->ui_set_active(objects.nodes_button, objects.nodes_panel, objects.top_nodes_panel);
    lv_obj_scroll_to_view(panel, LV_ANIM_ON);
}

void TFTView_320x240::removeSpinner(void)
{
    if (spinnerButton) {
        lv_obj_delete(spinnerButton);
        spinnerButton = nullptr;
        startTime = 0;
    }
}

void TFTView_320x240::ui_event_node_details(lv_event_t *e)
{
    THIS->ui_set_active(objects.settings_button, objects.details_panel, objects.top_nodes_panel);
}

void TFTView_320x240::ui_event_statistics(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        THIS->ui_set_active(objects.settings_button, objects.tools_statistics_panel, objects.top_statistics_panel);
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        // clear statistics table
        THIS->updateStatistics(meshtastic_MeshPacket{.from = 0});
    }
}

void TFTView_320x240::ui_event_packet_log(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        THIS->ui_set_active(objects.settings_button, objects.tools_packet_log_panel, objects.top_packet_log_panel);
        THIS->packetLogEnabled = true;
    } else if (event_code == LV_EVENT_LONG_PRESSED) {
        THIS->packetCounter = 0;
        lv_obj_clean(objects.tools_packet_log_panel);
    }
}

void TFTView_320x240::packetDetected(const meshtastic_MeshPacket &p)
{
    uint32_t heard = 0;
    if (p.from != ownNode)
        heard = p.from;
    if (p.to != 0xffffffff && p.to != ownNode)
        heard = p.to;

    if (heard) {
        if (p.to == ownNode && p.decoded.portnum == meshtastic_PortNum_NODEINFO_APP) {
            // we finally sensed a two-way contact to us; stop the detector
            detectorRunning = false;
            lv_label_set_text(objects.detector_start_label, _("Start"));
            lv_anim_del(&objects.radar_beam, ui_anim_radar_cb);
            lv_obj_add_flag(objects.detector_radar_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.detector_heard_label, LV_OBJ_FLAG_HIDDEN);

            setNodeImage(p.from, (MeshtasticView::eRole)(unsigned long)nodes[p.from]->LV_OBJ_IDX(node_img_idx)->user_data, false,
                         objects.detector_contact_image);
            const char *lbl = lv_label_get_text(nodes[p.from]->LV_OBJ_IDX(node_lbl_idx));

            char from[5];
            char *userShort = (char *)&(nodes[p.from]->LV_OBJ_IDX(node_lbs_idx)->user_data);
            int pos = 0;
            while (pos < 4 && userShort[pos] != 0) {
                from[pos] = userShort[pos];
                pos++;
            }
            from[pos] = '\0';

            char buf[64];
            lv_snprintf(buf, 64, "%s(%04x)\n%s", from, p.from & 0xffff, lbl);
            lv_label_set_text(objects.detector_contact_label, buf);
            lv_obj_clear_flag(objects.detector_contact_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            char buf[20];
            lv_snprintf(buf, 20, _("heard: !%08x"), heard);
            lv_label_set_text(objects.detector_heard_label, buf);
            lv_obj_clear_flag(objects.detector_heard_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TFTView_320x240::writePacketLog(const meshtastic_MeshPacket &p)
{
    static std::unordered_map<uint16_t, const char *> name = {
        {0, "unknown"},        {1, "text message"},    {2, "remote hardware"}, {3, "position"},    {4, "node info"},
        {5, "routing"},        {6, "admin"},           {7, "text message"},    {8, "waypoint"},    {9, "audio"},
        {10, "sensor"},        {32, "reply"},          {33, "ip tunnel"},      {34, "paxcounter"}, {64, "serial"},
        {65, "store forward"}, {66, "range test"},     {67, "telemetry"},      {68, "ZPS"},        {69, "simulator"},
        {70, "tracert"},       {71, "neighbor info"},  {72, "atax"},           {73, "map report"}, {74, "power stress"},
        {256, "private"},      {257, "atax forwarder"}};

    // ignore admin packages initiated by us
    if (p.from == ownNode && p.decoded.portnum == meshtastic_PortNum_ADMIN_APP)
        return;

    // get actual time
    char timebuf[16];
    time_t curr_time;
#ifdef ARCH_PORTDUINO
    time(&curr_time);
#else
    curr_time = actTime;
#endif
    tm *curr_tm = localtime(&curr_time);
    if (VALID_TIME(curr_time)) {
        strftime(timebuf, 16, "%T", curr_tm);
    } else {
        strcpy(timebuf, "??:??:??");
    }

    // get node name from
    char from[5];
    char *userShort = (char *)&(nodes[p.from]->LV_OBJ_IDX(node_lbs_idx)->user_data);
    int pos = 0;
    while (pos < 4 && userShort[pos] != 0) {
        from[pos] = userShort[pos];
        pos++;
    }
    from[pos] = '\0';

    char buf[256];
    if (p.to == 0xffffffff)
        sprintf(buf, "%s: ch%d %s:%04x->all: %s", timebuf, p.channel, from, p.from & 0xffff,
                name[p.decoded.portnum]); // note: this may crash if there's a new portnum not in this map...
    else
        sprintf(buf, "%s: ch%d %s:%04x->%s%04x: %s", timebuf, p.channel, from, p.from & 0xffff, p.to == ownNode ? "*" : "",
                p.to & 0xffff, name[p.decoded.portnum]);

    if (p.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP) {
        meshtastic_Telemetry telemetry;
        if (pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_Telemetry_msg, &telemetry)) {
            switch (telemetry.which_variant) {
            case meshtastic_Telemetry_device_metrics_tag: {
                if (p.from == ownNode)
                    return; // suppress (internal) battery level packets
                strcat(buf, " dev");
                break;
            }
            case meshtastic_Telemetry_environment_metrics_tag: {
                strcat(buf, " env");
                break;
            }
            case meshtastic_Telemetry_air_quality_metrics_tag: {
                strcat(buf, " air");
                break;
            }
            case meshtastic_Telemetry_power_metrics_tag: {
                strcat(buf, " pow");
                break;
            }
            case meshtastic_Telemetry_local_stats_tag: {
                strcat(buf, " dev"); // bug in firmware that this is local?
            }
            default:
                break;
            }
        }
    } else if (p.decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
        // print the recorded route and add from/to manually
        strcat(buf, "\n");
        int pos = strlen(buf);
        if (p.to == ownNode) {
            pos += snprintf(&buf[pos], 16, "%04x", ownNode & 0xffff);
        }

        meshtastic_RouteDiscovery route;
        if (pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_RouteDiscovery_msg, &route)) {
            for (int i = 0; i < route.route_count; i++) {
                uint32_t nodeNum = route.route[i];
                if (nodeNum != UINT32_MAX) {
                    pos += snprintf(&buf[pos], 16, "->%04x", nodeNum & 0xffff);
                } else {
                    strcat(buf, "->unk");
                    pos += 6;
                }
            }
            if (p.to == ownNode) {
                pos += snprintf(&buf[pos], 16, "->%04x", p.from & 0xffff);
            }
        }
    }

    if (packetCounter >= PACKET_LOGS_MAX) {
        // delete oldest entry
        lv_obj_del(objects.tools_packet_log_panel->spec_attr->children[0]);
    } else {
        packetCounter++;
        char top[24];
        sprintf(top, _("Packet Log: %d"), packetCounter);
        lv_label_set_text(objects.top_packet_log_label, top);
    }
    lv_obj_t *pLabel = lv_label_create(objects.tools_packet_log_panel);
    lv_obj_set_pos(pLabel, 0, 0);
    lv_obj_set_size(pLabel, LV_PCT(100), LV_SIZE_CONTENT);
    uint32_t bgColor, fgColor;
    std::tie(bgColor, fgColor) = nodeColor(p.from);
    lv_obj_set_style_bg_color(pLabel, lv_color_hex(bgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(pLabel, lv_color_hex(fgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(pLabel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(pLabel, buf);

    // auto-scroll if last item is visible
    if (lv_obj_get_scroll_bottom(objects.tools_packet_log_panel) < 20)
        lv_obj_scroll_to_view(pLabel, LV_ANIM_OFF);
}

void TFTView_320x240::updateStatistics(const meshtastic_MeshPacket &p)
{
    struct Stats {
        uint32_t id;
        uint16_t row;
        uint16_t tel;
        uint16_t pos;
        uint16_t inf;
        uint16_t trc;
        uint16_t txt;
        uint16_t nbr;
        uint32_t sum;

        bool operator==(const Stats &rhs) const { return id == rhs.id; }

        Stats &operator+=(const Stats &rhs)
        {
            this->tel += rhs.tel;
            this->pos += rhs.pos;
            this->inf += rhs.inf;
            this->trc += rhs.trc;
            this->txt += rhs.txt;
            this->nbr += rhs.nbr;
            this->sum += 1;
            return *this;
        }

        bool operator<(const Stats &rhs) const
        {
            return sum > rhs.sum; // sort reverse but skip equal values
        }
    };
    static std::list<Stats> stats;

    if (p.from == 0) {
        // clear table
        stats.clear();
        for (int i = 1; i < statisticTableRows; i++) {
            for (int j = 0; j < 7; j++) {
                lv_table_set_cell_value(objects.statistics_table, i, j, "");
            }
        }
        return;
    }

    // update statistic for node
    Stats stat = {p.from};
    switch (p.decoded.portnum) {
    case meshtastic_PortNum_TELEMETRY_APP: {
        meshtastic_Telemetry telemetry;
        if (pb_decode_from_bytes(p.decoded.payload.bytes, p.decoded.payload.size, &meshtastic_Telemetry_msg, &telemetry)) {
            if (telemetry.which_variant == meshtastic_Telemetry_device_metrics_tag) {
                if (p.from == ownNode)
                    return; // suppress (internal) battery level packets
            }
        }
        stat.tel++;
        break;
    }
    case meshtastic_PortNum_POSITION_APP: {
        stat.pos++;
        break;
    }
    case meshtastic_PortNum_NODEINFO_APP: {
        stat.inf++;
        break;
    }
    case meshtastic_PortNum_ROUTING_APP:
    case meshtastic_PortNum_TRACEROUTE_APP: {
        stat.trc++;
        break;
    }
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
    case meshtastic_PortNum_RANGE_TEST_APP: {
        stat.txt++;
        break;
    }
    case meshtastic_PortNum_NEIGHBORINFO_APP: {
        stat.nbr++;
        break;
    }
    case meshtastic_PortNum_ADMIN_APP: {
        // ignore
        break;
    }
    default:
        ILOG_WARN("packet portnum in stats unhandled: %d", p.decoded.portnum);
        stat.sum++;
        return;
    }

    std::list<Stats>::iterator it = std::find(stats.begin(), stats.end(), stat);
    if (it == stats.end()) {
        stat.row = stats.size();
        stat.sum = 1;
        // TODO: stop if memory limit is reached
        stats.push_back(stat);
    } else {
        *it += stat;
    }

    stats.sort();

    // fill packet statistics table
    char buf[10];
    int row = 1;
    bool move = false;

    for (auto it2 : stats) {
        if (it2.id == p.from || move) {
            buf[0] = '\0';
            auto it = nodes.find(it2.id); // node may have been removed from nodes, so check if still there
            if (it != nodes.end() && it->second) {
                char *userData = (char *)&(it->second->LV_OBJ_IDX(node_lbs_idx)->user_data);
                if (userData) {
                    buf[0] = userData[0];
                    buf[1] = userData[1];
                    buf[2] = userData[2];
                    buf[3] = userData[3];
                    buf[4] = '\0';
                }
            }

            lv_table_set_cell_value(objects.statistics_table, row, 0, buf);
            sprintf(buf, "%d", it2.tel);
            lv_table_set_cell_value(objects.statistics_table, row, 1, buf);
            sprintf(buf, "%d", it2.pos);
            lv_table_set_cell_value(objects.statistics_table, row, 2, buf);
            sprintf(buf, "%d", it2.inf);
            lv_table_set_cell_value(objects.statistics_table, row, 3, buf);
            sprintf(buf, "%d", it2.trc);
            lv_table_set_cell_value(objects.statistics_table, row, 4, buf);
            sprintf(buf, "%d", it2.nbr);
            lv_table_set_cell_value(objects.statistics_table, row, 5, buf);
            sprintf(buf, "%d", it2.sum);
            lv_table_set_cell_value(objects.statistics_table, row, 6, buf);

            if (row != it2.row) {
                it2.row = row;
                move = true;
            } else {
                break;
            }
        }
        row++;
        if (row >= statisticTableRows) // fill rows till bottom of 320x240 display
            break;
    }
}

void TFTView_320x240::ui_event_statistics_table(lv_event_t *e)
{
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    // if the cells are drawn...
    if (base_dsc->part == LV_PART_ITEMS) {
        // make the texts in the first cell blueish
        lv_draw_fill_dsc_t *fill_draw_dsc = lv_draw_task_get_fill_dsc(draw_task);
        if (fill_draw_dsc) {
            uint32_t row = base_dsc->id1;
            if (row == 0) {
                fill_draw_dsc->color = lv_color_mix(lv_palette_main(LV_PALETTE_BLUE), fill_draw_dsc->color, LV_OPA_20);
            }
            // make every 2nd row grayish
            else {
                Themes::recolorTableRow(fill_draw_dsc, row % 2 == 0);
            }
        }
    }
}

void TFTView_320x240::requestSetup(void)
{
    ui_set_active(objects.settings_button, objects.initial_setup_panel, objects.top_setup_panel);
    lv_dropdown_set_selected(objects.setup_region_dropdown, 0);
    lv_obj_clear_flag(objects.initial_setup_panel, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(objects.setup_region_dropdown);
    THIS->disablePanel(objects.controller_panel);
    THIS->activeSettings = eSetup;
}

/**
 * update signal strength text and image for home screen
 */
void TFTView_320x240::updateSignalStrength(int32_t rssi, float snr)
{
    // remember time we last heard a node
    time(&lastHeard);

    if (rssi != 0 || snr != 0.0) {
        char buf[40];
        sprintf(buf, "SNR: %.1f\nRSSI: %" PRId32, snr, rssi);
        lv_label_set_text(objects.home_signal_label, buf);

        uint32_t pct = signalStrength2Percent(rssi, snr);
        sprintf(buf, "(%d%%)", pct);
        lv_label_set_text(objects.home_signal_pct_label, buf);
        if (pct > 80) {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_signal_button_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (pct > 60) {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_strong_signal_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (pct > 40) {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_good_signal_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (pct > 20) {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_fair_signal_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
        } else if (pct > 1) {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_weak_signal_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_no_signal_image, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

/**
 * Translate proto modem preset enum value to numerical position in dropdown menu
 */
uint32_t TFTView_320x240::preset2val(meshtastic_Config_LoRaConfig_ModemPreset preset)
{
    int32_t val[] = {0, -1, -1, 4, 3, 7, 5, 1, 6, 2};

    if (preset > (sizeof(val) / sizeof(val[0]) - 1) || val[preset] == -1) {
        ILOG_WARN("unknown or deprecated preset value: %d", preset);
        return 0;
    }
    return uint32_t(val[preset]);
}

/**
 * Translate value from dropdown menu to modem preset proto enum
 */
meshtastic_Config_LoRaConfig_ModemPreset TFTView_320x240::val2preset(uint32_t val)
{
    meshtastic_Config_LoRaConfig_ModemPreset preset[] = {
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST,   meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE,
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO,  meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST,
        meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST,
        meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW};
    if (val > (sizeof(preset) / sizeof(preset[0]) - 1)) {
        ILOG_ERROR("unknown preset value: %d", val);
        return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    }
    return preset[val];
}

/**
 * Translate proto role enum value to numerical position in dropdown menu
 */
uint32_t TFTView_320x240::role2val(meshtastic_Config_DeviceConfig_Role role)
{
#ifdef USE_ROUTER_ROLE
    int32_t val[] = {0, 1, 2, -1, 3, 4, 5, 6, 7, 8, 9};
#else
    int32_t val[] = {0, 1, -1, -1, -1, 2, 3, 4, 5, 6, 7};
#endif
    if (role > 10 || val[role] == -1) {
        ILOG_WARN("unknown role value: %d", role);
        return 0;
    }
    return uint32_t(val[role]);
}

/**
 * Translate value from dropdown menu to role proto enum
 */
meshtastic_Config_DeviceConfig_Role TFTView_320x240::val2role(uint32_t val)
{
    meshtastic_Config_DeviceConfig_Role role[] = {meshtastic_Config_DeviceConfig_Role_CLIENT,
                                                  meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,
#ifdef USE_ROUTER_ROLE
                                                  meshtastic_Config_DeviceConfig_Role_ROUTER,
                                                  meshtastic_Config_DeviceConfig_Role_REPEATER,
#endif
                                                  meshtastic_Config_DeviceConfig_Role_TRACKER,
                                                  meshtastic_Config_DeviceConfig_Role_SENSOR,
                                                  meshtastic_Config_DeviceConfig_Role_TAK,
                                                  meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN,
                                                  meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND,
                                                  meshtastic_Config_DeviceConfig_Role_TAK_TRACKER,
                                                  meshtastic_Config_DeviceConfig_Role_ROUTER_LATE};
    if (val > 10) {
        ILOG_WARN("unknown role value: %d", val);
        return meshtastic_Config_DeviceConfig_Role_CLIENT;
    }
    return role[val];
}

/**
 * Translate language proto enum value to (alphabetical) position in dropdown menu
 */
uint32_t TFTView_320x240::language2val(meshtastic_Language lang)
{
    switch (lang) {
    case meshtastic_Language_ENGLISH:
        return 0;
    case meshtastic_Language_FRENCH:
        return 7;
    case meshtastic_Language_GERMAN:
        return 4;
    case meshtastic_Language_ITALIAN:
        return 8;
    case meshtastic_Language_PORTUGUESE:
        return 12;
    case meshtastic_Language_SPANISH:
        return 6;
    case meshtastic_Language_SWEDISH:
        return 17;
    case meshtastic_Language_FINNISH:
        return 16;
    case meshtastic_Language_POLISH:
        return 11;
    case meshtastic_Language_TURKISH:
        return 18;
    case meshtastic_Language_SERBIAN:
        return 15;
    case meshtastic_Language_RUSSIAN:
        return 13;
    case meshtastic_Language_DUTCH:
        return 9;
    case meshtastic_Language_GREEK:
        return 5;
    case meshtastic_Language_NORWEGIAN:
        return 10;
    case meshtastic_Language_SLOVENIAN:
        return 14;
    case meshtastic_Language_UKRAINIAN:
        return 19;
    case meshtastic_Language_BULGARIAN:
        return 1;
    case meshtastic_Language_CZECH:
        return 2;
    case meshtastic_Language_DANISH:
        return 3;
    case meshtastic_Language_SIMPLIFIED_CHINESE:
        return 20;
    case meshtastic_Language_TRADITIONAL_CHINESE:
        return 21;
    default:
        ILOG_WARN("unknown language uiconfig: %d", lang);
    }
    return 0;
}

/**
 * Translate value from dropdown menu to language proto enum
 */
meshtastic_Language TFTView_320x240::val2language(uint32_t val)
{
    switch (val) {
    case 0:
        return meshtastic_Language_ENGLISH;
    case 7:
        return meshtastic_Language_FRENCH;
    case 4:
        return meshtastic_Language_GERMAN;
    case 8:
        return meshtastic_Language_ITALIAN;
    case 12:
        return meshtastic_Language_PORTUGUESE;
    case 6:
        return meshtastic_Language_SPANISH;
    case 17:
        return meshtastic_Language_SWEDISH;
    case 16:
        return meshtastic_Language_FINNISH;
    case 11:
        return meshtastic_Language_POLISH;
    case 18:
        return meshtastic_Language_TURKISH;
    case 15:
        return meshtastic_Language_SERBIAN;
    case 13:
        return meshtastic_Language_RUSSIAN;
    case 9:
        return meshtastic_Language_DUTCH;
    case 5:
        return meshtastic_Language_GREEK;
    case 10:
        return meshtastic_Language_NORWEGIAN;
    case 14:
        return meshtastic_Language_SLOVENIAN;
    case 19:
        return meshtastic_Language_UKRAINIAN;
    case 1:
        return meshtastic_Language_BULGARIAN;
    case 2:
        return meshtastic_Language_CZECH;
    case 3:
        return meshtastic_Language_DANISH;
    case 20:
        return meshtastic_Language_SIMPLIFIED_CHINESE;
    case 21:
        return meshtastic_Language_TRADITIONAL_CHINESE;
    default:
        ILOG_WARN("unknown language val: %d", val);
    }
    return meshtastic_Language_ENGLISH;
}

/**
 * @brief Set lv_i18n language
 */
void TFTView_320x240::setLocale(meshtastic_Language lang)
{
    const char *locale = "en_US.UTF-8";
    switch (lang) {
    case meshtastic_Language_ENGLISH:
        lv_i18n_set_locale("en");
        break;
    case meshtastic_Language_BULGARIAN:
        lv_i18n_set_locale("bg");
        locale = "bg_BG.UTF-8";
        break;
    case meshtastic_Language_GERMAN:
        lv_i18n_set_locale("de");
        locale = "de_DE.UTF-8";
        break;
    case meshtastic_Language_SPANISH:
        lv_i18n_set_locale("es");
        locale = "es_ES.UTF-8";
        break;
    case meshtastic_Language_FRENCH:
        lv_i18n_set_locale("fr");
        locale = "fr_FR.UTF-8";
        break;
    case meshtastic_Language_ITALIAN:
        lv_i18n_set_locale("it");
        locale = "it_IT.UTF-8";
        break;
    case meshtastic_Language_PORTUGUESE:
        lv_i18n_set_locale("pt");
        locale = "pt_PT.UTF-8";
        break;
    case meshtastic_Language_SWEDISH:
        lv_i18n_set_locale("se");
        locale = "sv_SE.UTF-8";
        break;
    case meshtastic_Language_FINNISH:
        lv_i18n_set_locale("fi");
        locale = "fi_FI.UTF-8";
        break;
    case meshtastic_Language_POLISH:
        lv_i18n_set_locale("pl");
        locale = "pl_PL.UTF-8";
        break;
    case meshtastic_Language_TURKISH:
        lv_i18n_set_locale("tr");
        locale = "tr_TR.UTF-8";
        break;
    case meshtastic_Language_SERBIAN:
        lv_i18n_set_locale("sr");
        locale = "sr_RS.UTF-8";
        break;
    case meshtastic_Language_DUTCH:
        lv_i18n_set_locale("nl");
        locale = "nl_NL.UTF-8";
        break;
    case meshtastic_Language_RUSSIAN:
        lv_i18n_set_locale("ru");
        locale = "ru_RU.UTF-8";
        break;
    case meshtastic_Language_GREEK:
        lv_i18n_set_locale("el");
        locale = "el_GR.UTF-8";
        break;
    case meshtastic_Language_NORWEGIAN:
        lv_i18n_set_locale("no");
        locale = "no_NO.UTF-8";
        break;
    case meshtastic_Language_SLOVENIAN:
        lv_i18n_set_locale("sl");
        locale = "sl_SI.UTF-8";
        break;
    case meshtastic_Language_UKRAINIAN:
        lv_i18n_set_locale("uk");
        locale = "uk_UA.UTF-8";
        break;
    case meshtastic_Language_CZECH:
        lv_i18n_set_locale("cs");
        locale = "cs_CZ.UTF-8";
        break;
    case meshtastic_Language_DANISH:
        lv_i18n_set_locale("da");
        locale = "da_DK.UTF-8";
        break;
    case meshtastic_Language_SIMPLIFIED_CHINESE:
        lv_i18n_set_locale("cn");
        locale = "zh_CN.UTF-8";
        break;
    case meshtastic_Language_TRADITIONAL_CHINESE:
        lv_i18n_set_locale("tw");
        locale = "zh_TW.UTF-8";
        break;
    default:
        ILOG_WARN("Language %d not implemented", lang);
        break;
    }

#if defined(LOCALE_SUPPORT)
    std::locale::global(std::locale(locale));
#else
    (void)locale;
#endif
}

/**
 * @brief Set language (using dropdown strings)
 */
void TFTView_320x240::setLanguage(meshtastic_Language lang)
{
    char buf1[20], buf2[40];
    lv_dropdown_set_selected(objects.settings_language_dropdown, language2val(lang));
    lv_dropdown_get_selected_str(objects.settings_language_dropdown, buf1, sizeof(buf1));
    lv_snprintf(buf2, sizeof(buf2), _("Language: %s"), buf1);
    lv_label_set_text(objects.basic_settings_language_label, buf2);
}

/**
 * @brief Set timeout
 */
void TFTView_320x240::setTimeout(uint32_t timeout)
{
    char buf[32];
    if (timeout == 0)
        lv_snprintf(buf, sizeof(buf), _("Screen Timeout: off"));
    else
        lv_snprintf(buf, sizeof(buf), _("Screen Timeout: %ds"), timeout);
    lv_label_set_text(objects.basic_settings_timeout_label, buf);
    THIS->displaydriver->setScreenTimeout(timeout);
}

/**
 * @brief Set brightness
 */
void TFTView_320x240::setBrightness(uint32_t brightness)
{
    char buf[32];
    lv_snprintf(buf, sizeof(buf), _("Screen Brightness: %d%%"), uint16_t(round((brightness * 100) / 255.0)));
    lv_label_set_text(objects.basic_settings_brightness_label, buf);
    THIS->displaydriver->setBrightness((uint8_t)brightness);
}

/**
 * @brief Set theme to new value
 */
void TFTView_320x240::setTheme(uint32_t value)
{
    char buf1[30], buf2[30];
    lv_dropdown_set_selected(objects.settings_theme_dropdown, value);
    lv_dropdown_get_selected_str(objects.settings_theme_dropdown, buf1, sizeof(buf1));
    lv_snprintf(buf2, sizeof(buf2), _("Theme: %s"), buf1);
    lv_label_set_text(objects.basic_settings_theme_label, buf2);

    // change theme and redraw UI
    Themes::set(Themes::Theme(value));
    updateTheme();
}

/**
 * @brief Save all data from node options panel
 */
void TFTView_320x240::storeNodeOptions(void)
{
    // store node filter options
    meshtastic_NodeFilter &filter = db.uiConfig.node_filter;
    db.uiConfig.has_node_filter = true;
    filter.unknown_switch = lv_obj_has_state(objects.nodes_filter_unknown_switch, LV_STATE_CHECKED);
    filter.offline_switch = lv_obj_has_state(objects.nodes_filter_offline_switch, LV_STATE_CHECKED);
    filter.public_key_switch = lv_obj_has_state(objects.nodes_filter_public_key_switch, LV_STATE_CHECKED);
    // filter.channel = lv_dropdown_get_selected(objects.nodes_filter_channel_dropdown);
    filter.hops_away = lv_dropdown_get_selected(objects.nodes_filter_hops_dropdown);
    // filter.mqtt_switch = lv_obj_has_state(objects.nodes_filter_mqtt_switch, LV_STATE_CHECKED);
    filter.position_switch = lv_obj_has_state(objects.nodes_filter_position_switch, LV_STATE_CHECKED);
    strncpy(filter.node_name, lv_textarea_get_text(objects.nodes_filter_name_area), sizeof(filter.node_name));

    // store node highlight options
    meshtastic_NodeHighlight &highlight = db.uiConfig.node_highlight;
    db.uiConfig.has_node_highlight = true;
    highlight.chat_switch = lv_obj_has_state(objects.nodes_hl_active_chat_switch, LV_STATE_CHECKED);
    highlight.position_switch = lv_obj_has_state(objects.nodes_hl_position_switch, LV_STATE_CHECKED);
    highlight.telemetry_switch = lv_obj_has_state(objects.nodes_hl_telemetry_switch, LV_STATE_CHECKED);
    highlight.iaq_switch = lv_obj_has_state(objects.nodes_hliaq_switch, LV_STATE_CHECKED);
    strncpy(highlight.node_name, lv_textarea_get_text(objects.nodes_hl_name_area), sizeof(highlight.node_name));

    controller->storeUIConfig(db.uiConfig);
}

/**
 * @brief erase chat and all its resources
 */
void TFTView_320x240::eraseChat(uint32_t channelOrNode)
{
    if (chats.find(channelOrNode) == chats.end()) {
        ILOG_WARN("eraseChat: channelOrNode %d not found", channelOrNode);
        return;
    }
    if (channelOrNode < c_max_channels) {
        uint8_t ch = (uint8_t)channelOrNode;
        if (state == MeshtasticView::eRunning) {
            lv_obj_delete_delayed(chats.at(ch), 500);
        } else {
            lv_obj_del(chats.at(ch));
        }
        lv_obj_del(channelGroup.at(ch));
        channelGroup[ch] = nullptr;
        chats.erase(ch);
    } else {
        uint32_t nodeNum = channelOrNode;
        if (state == MeshtasticView::eRunning) {
            lv_obj_delete_delayed(chats.at(nodeNum), 500);
        } else {
            lv_obj_delete(chats.at(nodeNum));
        }
        lv_obj_del(messages.at(nodeNum));
        messages.erase(nodeNum);
        chats.erase(nodeNum);
    }
}

/**
 * @brief clears all (persistent) chat messages
 */
void TFTView_320x240::clearChatHistory(void)
{
    for (auto &it : chats) {
        lv_obj_delete(it.second);
        if (it.first < c_max_channels) {
            lv_obj_delete(channelGroup[it.first]);
            channelGroup[it.first] = nullptr;
        } else {
            lv_obj_delete(messages[it.first]);
        }
    }
    chats.clear();
    messages.clear();
    updateActiveChats();
    updateNodesFiltered(true);
    controller->removeTextMessages(0, 0, 0);
}

/**
 * @brief User widget OK button handling
 *
 * @param e
 */
void TFTView_320x240::ui_event_ok(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        switch (THIS->activeSettings) {
        case eSetup: {
            meshtastic_Config_LoRaConfig_RegionCode region =
                (meshtastic_Config_LoRaConfig_RegionCode)(lv_dropdown_get_selected(objects.setup_region_dropdown) + 1);

            uint32_t numChannels = LoRaPresets::getNumChannels(region, THIS->db.config.lora.modem_preset);
            // if (numChannels == 0) {
            //     // region not possible for selected preset, revert
            //     lv_dropdown_set_selected(objects.settings_region_dropdown, THIS->db.config.lora.region - 1);
            //     return;
            // }

            if (region != THIS->db.config.lora.region) {
                char buf1[10], buf2[30];
                lv_dropdown_get_selected_str(objects.setup_region_dropdown, buf1, sizeof(buf1));
                lv_snprintf(buf2, sizeof(buf2), _("Region: %s"), buf1);
                lv_label_set_text(objects.basic_settings_region_label, buf2);

                meshtastic_Config_LoRaConfig &lora = THIS->db.config.lora;
                uint32_t defaultSlot = lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ? lora.channel_num : 0;
                if (defaultSlot == 0) {
                    defaultSlot =
                        LoRaPresets::getDefaultSlot(region, THIS->db.config.lora.modem_preset, THIS->db.channel[0].settings.name);
                }
                lora.region = region;
                lora.channel_num = (defaultSlot <= numChannels ? defaultSlot : 1);
                THIS->controller->sendConfig(meshtastic_Config_LoRaConfig{lora}, THIS->ownNode);
            }

            char buf[30];
            const char *userShort = lv_textarea_get_text(objects.setup_user_short_textarea);
            const char *userLong = lv_textarea_get_text(objects.setup_user_long_textarea);
            if (strcmp(userShort, THIS->db.short_name) || strcmp(userLong, THIS->db.long_name)) {
                lv_snprintf(buf, sizeof(buf), _("User name: %s"), userShort);
                lv_label_set_text(objects.basic_settings_user_label, buf);
                lv_label_set_text(objects.user_name_short_label, userShort);
                lv_label_set_text(objects.user_name_label, userLong);
                strcpy(THIS->db.short_name, userShort);
                strcpy(THIS->db.long_name, userLong);
                meshtastic_User user{}; // TODO: don't overwrite is_licensed
                strcpy(user.short_name, userShort);
                strcpy(user.long_name, userLong);
                THIS->controller->sendConfig(user, THIS->ownNode);
            }
            THIS->notifyReboot(true);

            lv_obj_add_flag(objects.initial_setup_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.home_button);
            break;
        }
        case eUsername: {
            char buf[30];
            const char *userShort = lv_textarea_get_text(objects.settings_user_short_textarea);
            const char *userLong = lv_textarea_get_text(objects.settings_user_long_textarea);
            if (strcmp(userShort, THIS->db.short_name) || strcmp(userLong, THIS->db.long_name)) {
                lv_snprintf(buf, sizeof(buf), _("User name: %s"), userShort);
                lv_label_set_text(objects.basic_settings_user_label, buf);
                lv_label_set_text(objects.user_name_short_label, userShort);
                lv_label_set_text(objects.user_name_label, userLong);
                strcpy(THIS->db.short_name, userShort);
                strcpy(THIS->db.long_name, userLong);
                meshtastic_User user{}; // TODO: don't overwrite is_licensed
                strcpy(user.short_name, userShort);
                strcpy(user.long_name, userLong);
                THIS->controller->sendConfig(user, THIS->ownNode);
                THIS->notifyReboot(true);
            }
            lv_obj_add_flag(objects.settings_username_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_user_button);
            break;
        }
        case eDeviceRole: {
            meshtastic_Config_DeviceConfig &device = THIS->db.config.device;
            meshtastic_Config_DeviceConfig_Role role =
                THIS->val2role(lv_dropdown_get_selected(objects.settings_device_role_dropdown));

            if (role != device.role) {
                char buf1[30], buf2[40];
                lv_dropdown_get_selected_str(objects.settings_device_role_dropdown, buf1, sizeof(buf1));
                lv_snprintf(buf2, sizeof(buf2), _("Device Role: %s"), buf1);
                lv_label_set_text(objects.basic_settings_role_label, buf2);

                device.role = role;
                THIS->controller->sendConfig(meshtastic_Config_DeviceConfig{device}, THIS->ownNode);
                THIS->notifyReboot(true);
            }
            lv_obj_add_flag(objects.settings_device_role_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_role_button);
            break;
        }
        case eRegion: {
            meshtastic_Config_LoRaConfig_RegionCode region =
                (meshtastic_Config_LoRaConfig_RegionCode)(lv_dropdown_get_selected(objects.settings_region_dropdown) + 1);

            uint32_t numChannels = LoRaPresets::getNumChannels(region, THIS->db.config.lora.modem_preset);
            if (numChannels == 0) {
                // region not possible for selected preset, revert
                lv_dropdown_set_selected(objects.settings_region_dropdown, THIS->db.config.lora.region - 1);
                return;
            }

            if (region != THIS->db.config.lora.region) {
                char buf1[10], buf2[30];
                lv_dropdown_get_selected_str(objects.settings_region_dropdown, buf1, sizeof(buf1));
                lv_snprintf(buf2, sizeof(buf2), _("Region: %s"), buf1);
                lv_label_set_text(objects.basic_settings_region_label, buf2);

                meshtastic_Config_LoRaConfig &lora = THIS->db.config.lora;
                uint32_t defaultSlot = lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ? lora.channel_num : 0;
                if (defaultSlot == 0) {
                    defaultSlot =
                        LoRaPresets::getDefaultSlot(region, THIS->db.config.lora.modem_preset, THIS->db.channel[0].settings.name);
                }
                lora.region = region;
                lora.channel_num = (defaultSlot <= numChannels ? defaultSlot : 1);
                THIS->controller->sendConfig(meshtastic_Config_LoRaConfig{lora}, THIS->ownNode);
                THIS->notifyReboot(true);
            }
            lv_obj_add_flag(objects.settings_region_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_region_button);
            break;
        }
        case eModemPreset: {
            meshtastic_Config_LoRaConfig &lora = THIS->db.config.lora;
            meshtastic_Config_LoRaConfig_ModemPreset preset =
                THIS->val2preset(lv_dropdown_get_selected(objects.settings_modem_preset_dropdown));
            uint16_t channelNum = lv_slider_get_value(objects.frequency_slot_slider);
            if (preset != lora.modem_preset || lora.channel_num != channelNum) {
                char buf1[16], buf2[32];
                lv_dropdown_get_selected_str(objects.settings_modem_preset_dropdown, buf1, sizeof(buf1));
                lv_snprintf(buf2, sizeof(buf2), _("Modem Preset: %s"), buf1);
                lv_label_set_text(objects.basic_settings_modem_preset_label, buf2);

                lora.use_preset = true;
                lora.modem_preset = preset;
                lora.channel_num = channelNum;
                THIS->controller->sendConfig(meshtastic_Config_LoRaConfig{lora}, THIS->ownNode);
                THIS->notifyReboot(true);
            }
            lv_obj_add_flag(objects.settings_modem_preset_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_modem_preset_button);
            break;
        }
        case eChannel: {
            for (int i = 0; i < c_max_channels; i++) {
                // check if channel changed, then update and send to radio
                if (memcmp(&THIS->db.channel[i], &THIS->channel_scratch[i], sizeof(THIS->channel_scratch[i])) != 0) {
                    THIS->channel_scratch[i].has_settings = true;
                    THIS->updateChannelConfig(THIS->channel_scratch[i]);
                    THIS->controller->sendConfig(THIS->channel_scratch[i], THIS->ownNode);
                }
            }

            int8_t ch = (signed long)THIS->ch_label[0]->user_data;
            THIS->setChannelName(THIS->db.channel[ch]);
            lv_obj_clear_state(objects.settings_channel_panel, LV_STATE_DISABLED);
            lv_obj_add_flag(objects.settings_channel_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_channel_button);
            delete[] THIS->channel_scratch;
            break;
        }
        case eWifi: {
            char buf[30];
            const char *ssid = lv_textarea_get_text(objects.settings_wifi_ssid_textarea);
            const char *psk = lv_textarea_get_text(objects.settings_wifi_password_textarea);
            if (strlen(ssid) == 0 || strlen(psk) == 0)
                return;
            lv_snprintf(buf, sizeof(buf), _("WiFi: %s"), ssid[0] ? ssid : _("<not set>"));
            lv_label_set_text(objects.basic_settings_wifi_label, buf);
            if (strcmp(THIS->db.config.network.wifi_ssid, ssid) != 0 || strcmp(THIS->db.config.network.wifi_psk, psk) != 0) {
                strcpy(THIS->db.config.network.wifi_ssid, ssid);
                strcpy(THIS->db.config.network.wifi_psk, psk);
                THIS->db.config.network.wifi_enabled = true;
                THIS->controller->sendConfig(meshtastic_Config_NetworkConfig{THIS->db.config.network}, THIS->ownNode);
                THIS->notifyReboot(true);
            }
            THIS->enablePanel(objects.home_panel);
            lv_obj_add_flag(objects.settings_wifi_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_wifi_button);
            break;
        }
        case eLanguage: {
            uint32_t value = lv_dropdown_get_selected(objects.settings_language_dropdown);
            meshtastic_Language lang = THIS->val2language(value);
            if (lang != THIS->db.uiConfig.language) {
                THIS->db.uiConfig.language = lang;
                THIS->controller->storeUIConfig(THIS->db.uiConfig);
                THIS->controller->requestReboot(3, THIS->ownNode);
                THIS->notifyReboot(true);
            }

            lv_obj_add_flag(objects.settings_language_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_language_button);
            break;
        }
        case eScreenTimeout: {
            uint32_t value = lv_slider_get_value(objects.screen_timeout_slider);
            if (value > 5)
                value -= value % 5;
            if (value != THIS->db.uiConfig.screen_timeout) {
                THIS->setTimeout(value);
                THIS->db.uiConfig.screen_timeout = value;
                THIS->controller->storeUIConfig(THIS->db.uiConfig);
            }
            lv_obj_add_flag(objects.settings_screen_timeout_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_timeout_button);
            break;
        }
        case eScreenLock: {
            const char *pin = lv_textarea_get_text(objects.settings_screen_lock_password_textarea);
            bool screenLock = lv_obj_has_state(objects.settings_screen_lock_switch, LV_STATE_CHECKED);
            bool settingsLock = lv_obj_has_state(objects.settings_settings_lock_switch, LV_STATE_CHECKED);
            if ((screenLock || settingsLock) && (atol(pin) == 0 || strlen(pin) != 6))
                return; // require pin != "000000"
            if ((screenLock != THIS->db.uiConfig.screen_lock) || settingsLock != THIS->db.uiConfig.settings_lock ||
                atol(pin) != THIS->db.uiConfig.pin_code) {
                THIS->db.uiConfig.screen_lock = screenLock;
                THIS->db.uiConfig.settings_lock = settingsLock;
                THIS->db.uiConfig.pin_code = atol(pin);
                THIS->controller->storeUIConfig(THIS->db.uiConfig);
            }

            char buf[40];
            lv_snprintf(buf, 40, _("Lock: %s/%s"), screenLock ? _("on") : _("off"), settingsLock ? _("on") : _("off"));
            lv_label_set_text(objects.basic_settings_screen_lock_label, buf);
            lv_obj_add_flag(objects.settings_screen_lock_panel, LV_OBJ_FLAG_HIDDEN);

            break;
        }
        case eScreenBrightness: {
            int32_t value = lv_slider_get_value(objects.brightness_slider) * 255 / 100;
            if (value != THIS->db.uiConfig.screen_brightness) {
                THIS->setBrightness(value);
                THIS->db.uiConfig.screen_brightness = value;
                THIS->controller->storeUIConfig(THIS->db.uiConfig);
            }
            lv_obj_add_flag(objects.settings_brightness_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_brightness_button);
            break;
        }
        case eTheme: {
            uint32_t value = lv_dropdown_get_selected(objects.settings_theme_dropdown);
            if (value != THIS->db.uiConfig.theme) {
                THIS->setTheme(value);
                THIS->db.uiConfig.theme = meshtastic_Theme(value);
                THIS->controller->storeUIConfig(THIS->db.uiConfig);
                lv_obj_set_style_bg_img_recolor(objects.settings_button, colorMesh, LV_PART_MAIN | LV_STATE_DEFAULT);
            }

            lv_obj_add_flag(objects.settings_theme_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_theme_button);
            lv_obj_invalidate(objects.main_screen);
            break;
        }
        case eInputControl: {
            char new_val_kbd[10], new_val_ptr[10];
            lv_dropdown_get_selected_str(objects.settings_keyboard_input_dropdown, new_val_kbd, sizeof(new_val_kbd));
            lv_dropdown_get_selected_str(objects.settings_mouse_input_dropdown, new_val_ptr, sizeof(new_val_ptr));

            bool error = false;
            if (strcmp(THIS->old_val1_scratch, new_val_kbd) != 0) {
                if (strcmp(THIS->old_val1_scratch, _("none")) != 0) {
                    THIS->inputdriver->releaseKeyboardDevice();
                }
                if (strcmp(new_val_kbd, _("none")) != 0) {
                    error &= THIS->inputdriver->useKeyboardDevice(new_val_kbd);
                }
            }
            if (strcmp(THIS->old_val2_scratch, new_val_ptr) != 0) {
                if (strcmp(THIS->old_val2_scratch, _("none")) != 0) {
                    THIS->inputdriver->releasePointerDevice();
                }
                if (strcmp(new_val_ptr, _("none")) != 0) {
                    error &= THIS->inputdriver->usePointerDevice(new_val_ptr);
                }
            }

            THIS->setInputButtonLabel();

            if (error) {
                ILOG_WARN("failed to use %s/%s", new_val_kbd, new_val_ptr);
                return;
            }

            std::string current_kbd = THIS->inputdriver->getCurrentKeyboardDevice();
            std::string current_ptr = THIS->inputdriver->getCurrentPointerDevice();
            if (strcmp(current_kbd.c_str(), _("none")) == 0 && strcmp(current_ptr.c_str(), _("none")) == 0 && THIS->input_group) {
                lv_group_delete(THIS->input_group);
                THIS->input_group = nullptr;
            } else if (strcmp(THIS->old_val1_scratch, current_kbd.c_str()) != 0 ||
                       strcmp(THIS->old_val2_scratch, current_ptr.c_str()) != 0) {
                THIS->setInputGroup();
            }

            lv_obj_add_flag(objects.settings_input_control_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_input_button);
            break;
        }
        case eAlertBuzzer: {
            meshtastic_ModuleConfig_ExternalNotificationConfig &config = THIS->db.module_config.external_notification;
            int tone = lv_dropdown_get_selected(objects.settings_ringtone_dropdown) + 1;

            bool silent = false;
            bool alert_message = lv_obj_has_state(objects.settings_alert_buzzer_switch, LV_STATE_CHECKED);
            if ((!config.enabled || !config.alert_message_buzzer) && alert_message) {
                if (!config.enabled || !config.alert_message_buzzer || !config.use_pwm || !config.use_i2s_as_buzzer) {
                    config.enabled = true;
                    config.alert_message_buzzer = true;
                    config.use_pwm = true;
                    config.nag_timeout = 0;
#ifdef USE_I2S_BUZZER
                    config.use_i2s_as_buzzer = true;
                    config.use_pwm = false;
#endif
                }
                THIS->notifyReboot(true);
                THIS->controller->sendConfig(meshtastic_ModuleConfig_ExternalNotificationConfig{config}, THIS->ownNode);
            } else if (config.alert_message_buzzer && !alert_message) {
                silent = true;
            }

            THIS->controller->sendConfig(ringtone[silent ? 0 : tone].rtttl, THIS->ownNode);
            THIS->db.uiConfig.ring_tone_id = tone;
            THIS->db.silent = silent;
            THIS->db.uiConfig.alert_enabled = !silent;
            THIS->setBellText(THIS->db.uiConfig.alert_enabled, !silent);
            THIS->controller->storeUIConfig(THIS->db.uiConfig);

            lv_obj_add_flag(objects.settings_alert_buzzer_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_alert_button);
            break;
        }
        case eBackupRestore: {
            uint32_t option = lv_dropdown_get_selected(objects.settings_backup_restore_dropdown);
            if (lv_obj_has_state(objects.settings_backup_checkbox, LV_STATE_CHECKED)) {
                THIS->backup(option);
            } else if (lv_obj_has_state(objects.settings_restore_checkbox, LV_STATE_CHECKED)) {
                THIS->restore(option);
            }
            lv_obj_add_flag(objects.settings_backup_restore_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_backup_restore_button);
            break;
        }
        case eReset: {
            uint32_t option = lv_dropdown_get_selected(objects.settings_reset_dropdown);
            if (option == 2) {
                THIS->clearChatHistory();
            } else {
                THIS->notifyReboot(true);
                THIS->controller->requestReset(option, THIS->ownNode);
            }
            lv_obj_add_flag(objects.settings_reset_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_reset_button);
            break;
        }
        case eDisplayMode: {
            meshtastic_Config_DisplayConfig &display = THIS->db.config.display;
            meshtastic_Config_BluetoothConfig &bluetooth = THIS->db.config.bluetooth;
            display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_DEFAULT;
            bluetooth.enabled = true;
            THIS->controller->sendConfig(meshtastic_Config_DisplayConfig{display}, THIS->ownNode);
            THIS->controller->sendConfig(meshtastic_Config_BluetoothConfig{bluetooth}, THIS->ownNode);
            THIS->controller->requestReboot(5, THIS->ownNode);
            lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 4000, 1000, false);
            lv_obj_add_flag(objects.reboot_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(objects.settings_reboot_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case eModifyChannel: {
            meshtastic_ChannelSettings_psk_t psk = {};
            const char *name = lv_textarea_get_text(objects.settings_modify_channel_name_textarea);
            const char *base64 = lv_textarea_get_text(objects.settings_modify_channel_psk_textarea);
            uint8_t btn_id = (unsigned long)objects.settings_modify_channel_name_textarea->user_data;
            int8_t ch = (signed long)THIS->ch_label[btn_id]->user_data;

            if (strlen(base64) == 0 && strlen(name) == 0) {
                // delete channel
                THIS->channel_scratch[ch].role = meshtastic_Channel_Role_DISABLED;
                THIS->channel_scratch[ch].settings.psk.size = 0;
                memset(THIS->channel_scratch[ch].settings.name, 0, sizeof(THIS->channel_scratch[ch].settings.name));
                memset(THIS->channel_scratch[ch].settings.psk.bytes, 0, sizeof(THIS->channel_scratch[ch].settings.psk.bytes));
                THIS->channel_scratch[ch].has_settings = false;
                lv_label_set_text(THIS->ch_label[btn_id], _("<unset>"));
                THIS->activeSettings = eChannel;
            } else {
                int paddings = (4 - strlen(base64) % 4) % 4;
                while (paddings-- > 0) {
                    lv_textarea_add_text(objects.settings_modify_channel_psk_textarea, "=");
                }

                if (THIS->base64ToPsk(lv_textarea_get_text(objects.settings_modify_channel_psk_textarea), psk.bytes, psk.size)) {
                    if (strlen(name) || psk.size) {
                        // TODO: fill temp storage -> user data
                        lv_label_set_text(THIS->ch_label[btn_id], name);
                        strcpy(THIS->channel_scratch[ch].settings.name, name);
                        memcpy(THIS->channel_scratch[ch].settings.psk.bytes, psk.bytes, 32);
                        THIS->channel_scratch[ch].settings.psk.size = psk.size;
                        THIS->activeSettings = eChannel;
                    }
                }
                THIS->channel_scratch[ch].role = (ch == 0) ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
            }
            if (THIS->activeSettings == eChannel) {
                lv_obj_add_flag(objects.settings_modify_channel_panel, LV_OBJ_FLAG_HIDDEN);
                THIS->enablePanel(objects.settings_channel_panel);
                lv_group_focus_obj(objects.settings_channel0_button);
            }
            return;
        }
        default:
            ILOG_ERROR("Unhandled ok event");
            break;
        }
        THIS->enablePanel(objects.controller_panel);
        THIS->enablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eNone;
    }
}

/**
 * @brief Cancel button user widget handling
 *
 * @param e
 */
void TFTView_320x240::ui_event_cancel(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        switch (THIS->activeSettings) {
        case TFTView_320x240::eSetup: {
            THIS->ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
            // lv_obj_add_flag(objects.initial_setup_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.home_button);
            break;
        }
        case TFTView_320x240::eUsername: {
            lv_obj_add_flag(objects.settings_username_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_user_button);
            break;
        }
        case TFTView_320x240::eDeviceRole: {
            lv_obj_add_flag(objects.settings_device_role_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_role_button);
            break;
        }
        case TFTView_320x240::eRegion: {
            lv_obj_add_flag(objects.settings_region_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_region_button);
            break;
        }
        case TFTView_320x240::eModemPreset: {
            lv_obj_add_flag(objects.settings_modem_preset_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_modem_preset_button);
            break;
        }
        case TFTView_320x240::eChannel: {
            delete[] THIS->channel_scratch;
            lv_obj_add_flag(objects.settings_channel_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_channel_button);
            break;
        }
        case TFTView_320x240::eWifi: {
            lv_obj_add_flag(objects.settings_wifi_panel, LV_OBJ_FLAG_HIDDEN);
            THIS->enablePanel(objects.home_panel);
            lv_group_focus_obj(objects.home_wlan_button);
            break;
        }
        case TFTView_320x240::eLanguage: {
            lv_obj_add_flag(objects.settings_language_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_language_button);
            break;
        }
        case TFTView_320x240::eScreenTimeout: {
            lv_obj_add_flag(objects.settings_screen_timeout_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_timeout_button);
            break;
        }
        case eScreenLock: {
            lv_obj_add_flag(objects.settings_screen_lock_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_screen_lock_button);
            break;
        }
        case TFTView_320x240::eScreenBrightness: {
            lv_obj_add_flag(objects.settings_brightness_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_brightness_button);
            // revert to old brightness value
            uint32_t old_brightness = THIS->db.uiConfig.screen_brightness;
            THIS->displaydriver->setBrightness((uint8_t)old_brightness);
            break;
        }
        case TFTView_320x240::eTheme: {
            lv_obj_add_flag(objects.settings_theme_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_theme_button);
            break;
        }
        case TFTView_320x240::eInputControl: {
            lv_obj_add_flag(objects.settings_input_control_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_input_button);
            break;
        }
        case TFTView_320x240::eAlertBuzzer: {
            lv_obj_add_flag(objects.settings_alert_buzzer_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_alert_button);
            break;
        }
        case TFTView_320x240::eBackupRestore: {
            lv_obj_add_flag(objects.settings_backup_restore_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_backup_restore_button);
            break;
        }
        case TFTView_320x240::eReset: {
            lv_obj_add_flag(objects.settings_reset_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_reset_button);
            break;
        }
        case TFTView_320x240::eDisplayMode: {
            lv_obj_add_flag(objects.settings_reboot_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.basic_settings_reset_button);
            break;
        }
        case TFTView_320x240::eModifyChannel: {
            lv_obj_add_flag(objects.settings_modify_channel_panel, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(objects.settings_channel0_button);
            THIS->enablePanel(objects.settings_channel_panel);
            THIS->activeSettings = eChannel;
            return;
        }
        default:
            ILOG_ERROR("Unhandled cancel event");
            break;
        }

        THIS->enablePanel(objects.controller_panel);
        THIS->enablePanel(objects.tab_page_basic_settings);
        THIS->activeSettings = eNone;
    }
}

// end button event handlers

void TFTView_320x240::ui_event_screen_timeout_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    char buf[20];
    uint32_t value = lv_slider_get_value(slider);
    if (value > 5)
        value -= value % 5;
    if (value == 0)
        lv_snprintf(buf, sizeof(buf), _("Timeout: off"));
    else
        lv_snprintf(buf, sizeof(buf), _("Timeout: %ds"), value);
    lv_label_set_text(objects.settings_screen_timeout_label, buf);
}

void TFTView_320x240::ui_event_brightness_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    char buf[20];
    lv_snprintf(buf, sizeof(buf), _("Brightness: %d%%"), (int)lv_slider_get_value(slider));
    lv_label_set_text(objects.settings_brightness_label, buf);
    THIS->displaydriver->setBrightness((uint8_t)(lv_slider_get_value(slider) * 255 / 100));
}

void TFTView_320x240::ui_event_frequency_slot_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);
    char buf[40];
    uint32_t channel = (uint32_t)lv_slider_get_value(slider);
    sprintf(buf, _("FrequencySlot: %d (%g MHz)"), channel,
            LoRaPresets::getRadioFreq(THIS->db.config.lora.region,
                                      THIS->val2preset(lv_dropdown_get_selected(objects.settings_modem_preset_dropdown)),
                                      channel));
    lv_label_set_text(objects.frequency_slot_label, buf);
}

void TFTView_320x240::ui_event_modem_preset_dropdown(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target_obj(e);
    meshtastic_Config_LoRaConfig_ModemPreset preset =
        (meshtastic_Config_LoRaConfig_ModemPreset)lv_dropdown_get_selected(dropdown);
    uint32_t numChannels = LoRaPresets::getNumChannels(THIS->db.config.lora.region, preset);
    if (numChannels == 0) {
        // preset not possible for this region, revert
        lv_dropdown_set_selected(dropdown, THIS->db.config.lora.modem_preset);
        numChannels = LoRaPresets::getNumChannels(THIS->db.config.lora.region, THIS->db.config.lora.modem_preset);
        return;
    }

    uint32_t channel = LoRaPresets::getDefaultSlot(THIS->db.config.lora.region, preset, THIS->db.channel[0].settings.name);
    if (channel > numChannels)
        channel = 1;
    lv_slider_set_range(objects.frequency_slot_slider, 1, numChannels);
    lv_slider_set_value(objects.frequency_slot_slider, channel, LV_ANIM_ON);

    char buf[40];
    sprintf(buf, _("FrequencySlot: %d (%g MHz)"), channel,
            LoRaPresets::getRadioFreq(THIS->db.config.lora.region, preset, channel));
    lv_label_set_text(objects.frequency_slot_label, buf);
}

void TFTView_320x240::ui_event_setup_region_dropdown(lv_event_t *e) {}

// animations
void TFTView_320x240::ui_anim_node_panel_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, v);
}

void TFTView_320x240::ui_anim_radar_cb(void *var, int32_t r)
{
    lv_img_set_angle(objects.radar_beam, r);
}

/**
 * @brief Dynamically show user widget
 *        First a panel is created where the widget is located in, then the widget is drawn.
 *        "active_widget" contains the surrounding panel which must be destroyed
 *        to remove the widget from the screen (e.g. by pressing OK/Cancel).
 *
 * @param func
 */
void TFTView_320x240::showUserWidget(UserWidgetFunc createWidget)
{
    lv_obj_t *obj = lv_obj_create(objects.main_screen);
    lv_obj_set_pos(obj, 39, 25);
    lv_obj_set_size(obj, LV_PCT(88), LV_PCT(90));
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, colorDarkGray, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    activeWidget = obj;

    createWidget(activeWidget, NULL, 0);
}

void TFTView_320x240::handleAddMessage(char *msg)
{
    // retrieve nodeNum + channel from activeMsgContainer
    uint32_t to = UINT32_MAX;
    uint8_t ch = 0;
    uint8_t hopLimit = db.config.lora.hop_limit;
    uint32_t requestId;
    uint32_t channelOrNode = (unsigned long)activeMsgContainer->user_data;
    bool usePkc = false;

    auto callback = [this](const ResponseHandler::Request &req, ResponseHandler::EventType evt, int32_t pass) {
        this->onTextMessageCallback(req, evt, pass);
    };

    if (channelOrNode < c_max_channels) {
        ch = (uint8_t)channelOrNode;
        requestId = requests.addRequest(ch, ResponseHandler::TextMessageRequest, (void *)(long)ch, callback);
    } else {
        ch = (uint8_t)(unsigned long)nodes[channelOrNode]->user_data;
        to = channelOrNode;
        usePkc = (unsigned long)nodes[to]->LV_OBJ_IDX(node_bat_idx)->user_data; // hasKey
        requestId = requests.addRequest(to, ResponseHandler::TextMessageRequest, (void *)to, callback);
        // trial: hoplimit optimization for direct text messages
        int8_t hopsAway = (signed long)nodes[to]->LV_OBJ_IDX(node_sig_idx)->user_data;
        if (hopsAway < 0)
            hopsAway = db.config.lora.hop_limit;
        hopLimit = (hopsAway < db.config.lora.hop_limit ? hopsAway + 1 : hopsAway);
    }

    // tweak to allow multiple lines in single line text area
    for (int i = 0; i < strlen(msg); i++)
        if (msg[i] == CR_REPLACEMENT)
            msg[i] = '\n';

    controller->sendTextMessage(to, ch, hopLimit, actTime, requestId, usePkc, msg);
    addMessage(activeMsgContainer, actTime, requestId, msg, LogMessage::eNone);
}

/**
 * display message that has just been written and sent out
 */
void TFTView_320x240::addMessage(lv_obj_t *container, uint32_t msgTime, uint32_t requestId, char *msg,
                                 LogMessage::MsgStatus status)
{
    lv_obj_t *hiddenPanel = lv_obj_create(container);
    lv_obj_set_width(hiddenPanel, lv_pct(100));
    lv_obj_set_height(hiddenPanel, LV_SIZE_CONTENT);
    lv_obj_set_align(hiddenPanel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(hiddenPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    add_style_panel_style(hiddenPanel);

    lv_obj_set_style_border_width(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    hiddenPanel->user_data = (void *)requestId;

    // add timestamp
    char buf[284]; // 237 + 4 + 40 + 2 + 1
    buf[0] = '\0';
    uint32_t len = timestamp(buf, msgTime, status == LogMessage::eNone);
    strcat(&buf[len], msg);

    lv_obj_t *textLabel = lv_label_create(hiddenPanel);
    // calculate expected size of text bubble, to make it look nicer
    lv_coord_t width = lv_txt_get_width(buf, strlen(buf), &ui_font_montserrat_12, 0);
    lv_obj_set_width(textLabel, std::max<int32_t>(std::min<int32_t>(width, 200) + 10, 40));
    lv_obj_set_height(textLabel, LV_SIZE_CONTENT);
    lv_obj_set_y(textLabel, 0);
    lv_obj_set_align(textLabel, LV_ALIGN_RIGHT_MID);
    lv_label_set_text(textLabel, buf);

    add_style_chat_message_style(textLabel);

    lv_obj_scroll_to_view(hiddenPanel, LV_ANIM_ON);
    lv_obj_move_foreground(objects.message_input_area);

    switch (status) {
    case LogMessage::eHeard:
        lv_obj_set_style_border_color(textLabel, colorYellow, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    case LogMessage::eAcked:
        lv_obj_set_style_border_color(textLabel, colorBlueGreen, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    case LogMessage::eFailed:
        lv_obj_set_style_border_color(textLabel, colorRed, LV_PART_MAIN | LV_STATE_DEFAULT);
        break;
    default:
        break;
    }
}

void TFTView_320x240::addNode(uint32_t nodeNum, uint8_t ch, const char *userShort, const char *userLong, uint32_t lastHeard,
                              eRole role, bool hasKey, bool unmessagable)
{
    // lv_obj nodesPanel children  |  user data (4 bytes)
    // ==================================================
    // [0]: img                    | role
    // [1]: btn                    | ll group
    // [2]: lbl user long          | nodeNum
    // [3]: lbl user short         | userShort (4 chars)
    // [4]: lbl battery            | hasKey
    // [5]: lbl lastHeard          | lastHeard / curtime
    // [6]: lbl signal (or hops)   | hops away
    // [7]: lbl position 1         | lat
    // [8]: lbl position 2         | lon
    // [9]: lbl telemetry 1        |
    // [10]: lbl telemetry 2       | iaq
    // panel user_data: ch

    ILOG_DEBUG("addNode(%d): num=0x%08x, lastseen=%d, name=%s(%s), role=%d", nodeCount, nodeNum, lastHeard, userLong, userShort,
               role);
    while (nodeCount >= MAX_NUM_NODES_VIEW) {
        purgeNode(nodeNum);
    }

    lv_obj_t *p = lv_obj_create(objects.nodes_panel);
    lv_ll_t *lv_group_ll = &lv_group_get_default()->obj_ll;

    p->user_data = (void *)(uint32_t)ch;
    nodes[nodeNum] = p;
    nodeCount++;

    // NodePanel
    lv_obj_set_pos(p, LV_PCT(0), 0);
    lv_obj_set_size(p, LV_PCT(100), 53);
    lv_obj_set_align(p, LV_ALIGN_CENTER);
    lv_obj_set_style_pad_top(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(p, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                        LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLLABLE));
    add_style_node_panel_style(p);

    // NodeImage
    lv_obj_t *img = lv_img_create(p);
    setNodeImage(nodeNum, role, unmessagable, img);
    lv_obj_set_pos(img, -5, 3);
    lv_obj_set_size(img, 32, 32);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(img, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(img, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (!hasKey) {
        lv_obj_set_style_border_color(img, colorRed, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (unmessagable) {
        // node role icon is not clickable and replaced with a cancelled icon
        img->user_data = (void *)eRole::unmessagable;
    } else {
        img->user_data = (void *)role;
    }

    // NodeButton
    lv_obj_t *nodeButton = lv_btn_create(p);
    lv_obj_set_pos(nodeButton, 0, 0);
    lv_obj_set_size(nodeButton, LV_PCT(106), LV_PCT(100));
    add_style_node_button_style(nodeButton);
    lv_obj_set_align(nodeButton, LV_ALIGN_CENTER);
    lv_obj_add_flag(nodeButton, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_shadow_width(nodeButton, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_max_height(nodeButton, 132, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_min_height(nodeButton, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
    nodeButton->user_data = _lv_ll_get_tail(lv_group_ll);

    // UserNameLabel
    lv_obj_t *ln_lbl = lv_label_create(p);
    lv_obj_set_pos(ln_lbl, -5, 35);
    lv_obj_set_size(ln_lbl, LV_PCT(80), LV_SIZE_CONTENT);
    lv_label_set_long_mode(ln_lbl, LV_LABEL_LONG_SCROLL);
    lv_label_set_text(ln_lbl, userLong);
    ln_lbl->user_data = (void *)nodeNum;
    lv_obj_set_style_align(ln_lbl, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);

    // UserNameShortLabel
    lv_obj_t *sn_lbl = lv_label_create(p);
    lv_obj_set_pos(sn_lbl, 30, 10);
    lv_obj_set_size(sn_lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_long_mode(sn_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_align(sn_lbl, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(sn_lbl, &ui_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    // if short name contains only non-printable glyphs replace with short id
    if (lv_txt_get_width(userShort, strlen(userShort), &ui_font_montserrat_14, 0) <= 4) {
        lv_label_set_text_fmt(sn_lbl, "%04x", nodeNum & 0xffff);
    } else {
        lv_label_set_text(sn_lbl, userShort);
    }
    char *modUserShort = lv_label_get_text(sn_lbl);

    // keep a copy of the (4-byte) short name for use in many other widgets
    char *userData = (char *)&(sn_lbl->user_data);
    userData[0] = modUserShort[0];
    if (userData[0] == 0x00)
        userData[0] = ' ';
    userData[1] = modUserShort[1];
    if (userData[1] == 0x00)
        userData[1] = ' ';
    userData[2] = modUserShort[2];
    if (userData[2] == 0x00)
        userData[2] = ' ';
    userData[3] = modUserShort[3];
    if (userData[3] == 0x00)
        userData[3] = ' ';

    //  BatteryLabel
    lv_obj_t *ui_BatteryLabel = lv_label_create(p);
    lv_obj_set_pos(ui_BatteryLabel, 8, 17);
    lv_obj_set_size(ui_BatteryLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_BatteryLabel, LV_ALIGN_TOP_RIGHT);
    lv_label_set_text(ui_BatteryLabel, "");
    lv_obj_set_style_text_align(ui_BatteryLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_BatteryLabel->user_data = (void *)hasKey;
    // LastHeardLabel
    lv_obj_t *ui_lastHeardLabel = lv_label_create(p);
    lv_obj_set_pos(ui_lastHeardLabel, 8, 33);
    lv_obj_set_size(ui_lastHeardLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(ui_lastHeardLabel, LV_ALIGN_TOP_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(ui_lastHeardLabel, LV_LABEL_LONG_CLIP);

    // TODO: devices without actual time will report all nodes as lastseen = now
    if (lastHeard) {
        lastHeard = std::min(curtime, (time_t)lastHeard); // adapt values too large

        char buf[20];
        bool isOnline = lastHeardToString(lastHeard, buf);
        lv_label_set_text(ui_lastHeardLabel, buf);
        if (isOnline) {
            nodesOnline++;
        }
    } else {
        lv_label_set_text(ui_lastHeardLabel, "");
    }

    lv_obj_set_style_text_align(ui_lastHeardLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_lastHeardLabel->user_data = (void *)lastHeard;
    // SignalLabel / hopsAway
    lv_obj_t *ui_SignalLabel = lv_label_create(p);
    lv_obj_set_width(ui_SignalLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_SignalLabel, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui_SignalLabel, 8, 1);
    lv_obj_set_align(ui_SignalLabel, LV_ALIGN_TOP_RIGHT);
    lv_label_set_text(ui_SignalLabel, "");
    ui_SignalLabel->user_data = (void *)-1; // TODO viaMqtt; // used for filtering (applyNodesFilter)
    // PositionLabel
    lv_obj_t *ui_PositionLabel = lv_label_create(p);
    lv_obj_set_pos(ui_PositionLabel, -5, 49);
    lv_obj_set_size(ui_PositionLabel, 120, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_PositionLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui_PositionLabel, "");
    lv_obj_set_style_align(ui_PositionLabel, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_PositionLabel, colorBlueGreen, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_PositionLabel->user_data = 0; // store latitude
    // Position2Label
    lv_obj_t *ui_Position2Label = lv_label_create(p);
    lv_obj_set_pos(ui_Position2Label, -5, 63);
    lv_obj_set_size(ui_Position2Label, 108, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_Position2Label, LV_LABEL_LONG_SCROLL);
    lv_label_set_text(ui_Position2Label, "");
    lv_obj_set_style_align(ui_Position2Label, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_Position2Label->user_data = 0; // store longitude
    // Telemetry1Label
    lv_obj_t *ui_Telemetry1Label = lv_label_create(p);
    lv_obj_set_pos(ui_Telemetry1Label, 8, 49);
    lv_obj_set_size(ui_Telemetry1Label, 130, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_Telemetry1Label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui_Telemetry1Label, "");
    lv_obj_set_style_align(ui_Telemetry1Label, LV_ALIGN_TOP_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Telemetry1Label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    // Telemetry2Label
    lv_obj_t *ui_Telemetry2Label = lv_label_create(p);
    lv_obj_set_pos(ui_Telemetry2Label, 8, 63);
    lv_obj_set_size(ui_Telemetry2Label, 130, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_Telemetry2Label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui_Telemetry2Label, "");
    lv_obj_set_style_align(ui_Telemetry2Label, LV_ALIGN_TOP_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Telemetry2Label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);

    // optimisation: hide all 6ix extended labels by default; enable only when set
    // lv_obj_add_flag(ui_lastHeardLabel, LV_OBJ_FLAG_HIDDEN); // lastHeard
    lv_obj_add_flag(ui_BatteryLabel, LV_OBJ_FLAG_HIDDEN); // Autohide battery
    lv_obj_add_flag(ui_SignalLabel, LV_OBJ_FLAG_HIDDEN);  // Autohide signal/hops
    lv_obj_add_flag(ui_PositionLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Position2Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Telemetry1Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Telemetry2Label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(nodeButton, ui_event_NodeButton, LV_EVENT_ALL, (void *)nodeNum);

    // move node into new position within nodePanel
    if (lastHeard) {
        lv_obj_t **children = objects.nodes_panel->spec_attr->children;
        int i = objects.nodes_panel->spec_attr->child_cnt - 1;
        while (i > 1) {
            if (lastHeard <= (time_t)(children[i - 1]->LV_OBJ_IDX(node_lh_idx)->user_data))
                break;
            i--;
        }
        if (i >= 1 && i < objects.nodes_panel->spec_attr->child_cnt - 1) {
            lv_obj_move_to_index(p, i);
            // re-arrange the group linked list by moving the new button (now at the tail) into the right position
            void *after = children[i + 1]->LV_OBJ_IDX(node_btn_idx)->user_data;
            _lv_ll_move_before(lv_group_ll, nodeButton->user_data, after);
        }
    }

    if (!nodesChanged) {
        applyNodesFilter(nodeNum);
        updateNodesStatus();
    }
}

void TFTView_320x240::setMyInfo(uint32_t nodeNum)
{
    ownNode = nodeNum;
}

void TFTView_320x240::setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                                        bool can_shutdown)
{
}

void TFTView_320x240::addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg)
{
    if (nodes.find(nodeNum) == nodes.end()) {
        addNode(nodeNum, channel, cfg.short_name, cfg.long_name, lastHeard, (MeshtasticView::eRole)cfg.role,
                cfg.public_key.size != 0, cfg.has_is_unmessagable && cfg.is_unmessagable);
    } else {
        updateNode(nodeNum, channel, cfg);
    }
}

/**
 * @brief update node userName and image
 *
 * @param nodeNum
 * @param ch
 * @param userShort
 * @param userLong
 * @param lastHeard
 * @param role
 * @param viaMqtt
 */
// void TFTView_320x240::updateNode(uint32_t nodeNum, uint8_t ch, const char *userShort, const char *userLong, uint32_t lastHeard,
//                                  eRole role, bool hasKey, bool viaMqtt)
void TFTView_320x240::updateNode(uint32_t nodeNum, uint8_t ch, const meshtastic_User &cfg)
{
    db.user = cfg;
    auto it = nodes.find(nodeNum);
    if (it != nodes.end() && it->second) {
        if (it->first == ownNode) {
            // update related settings buttons and store role in image user data
            char buf[30];
            lv_snprintf(buf, sizeof(buf), _("User name: %s"), cfg.short_name);
            lv_label_set_text(objects.basic_settings_user_label, buf);

            char buf1[30], buf2[40];
            lv_dropdown_set_selected(objects.settings_device_role_dropdown,
                                     role2val(meshtastic_Config_DeviceConfig_Role(cfg.role)));
            lv_dropdown_get_selected_str(objects.settings_device_role_dropdown, buf1, sizeof(buf1));
            lv_snprintf(buf2, sizeof(buf2), _("Device Role: %s"), buf1);
            lv_label_set_text(objects.basic_settings_role_label, buf2);

            // update DB
            strcpy(db.short_name, cfg.short_name);
            strcpy(db.long_name, cfg.long_name);
            db.config.device.role = cfg.role;
        }
        lv_label_set_text(it->second->LV_OBJ_IDX(node_lbl_idx), cfg.long_name);
        it->second->LV_OBJ_IDX(node_lbl_idx)->user_data = (void *)nodeNum;
        lv_label_set_text(it->second->LV_OBJ_IDX(node_lbs_idx), cfg.short_name);
        char *userData = (char *)&(it->second->LV_OBJ_IDX(node_lbs_idx)->user_data);
        userData[0] = cfg.short_name[0];
        if (userData[0] == 0x00)
            userData[0] = ' ';
        userData[1] = cfg.short_name[1];
        if (userData[1] == 0x00)
            userData[1] = ' ';
        userData[2] = cfg.short_name[2];
        if (userData[2] == 0x00)
            userData[2] = ' ';
        userData[3] = cfg.short_name[3];
        if (userData[3] == 0x00)
            userData[3] = ' ';

        setNodeImage(nodeNum, (MeshtasticView::eRole)cfg.role, cfg.has_is_unmessagable && cfg.is_unmessagable,
                     it->second->LV_OBJ_IDX(node_img_idx));

        if (cfg.public_key.size != 0) {
            // set border color to bg color
            lv_color_t color = lv_obj_get_style_bg_color(it->second->LV_OBJ_IDX(node_img_idx), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(it->second->LV_OBJ_IDX(node_img_idx), color, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_border_color(it->second->LV_OBJ_IDX(node_img_idx), colorRed, LV_PART_MAIN | LV_STATE_DEFAULT);
        }

        // update chat name
        auto ct = chats.find(it->first);
        if (ct != chats.end()) {
            char buf[64];
            lv_snprintf(buf, sizeof(buf), "%s: %s", lv_label_get_text(it->second->LV_OBJ_IDX(node_lbs_idx)),
                        lv_label_get_text(it->second->LV_OBJ_IDX(node_lbl_idx)));
            lv_label_set_text(ct->second->spec_attr->children[0], buf);
        }
    }
}

void TFTView_320x240::updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision)
{
    int32_t altU = abs(alt) < 10000 ? alt : 0;
    char units[3] = {};
    if (db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC) {
        units[0] = 'm';
    } else {
        units[0] = 'f';
        units[1] = 't';
        altU = int32_t(float(altU) * 3.28084);
    }
    if (nodeNum == ownNode) {
        char buf[64];
        int latSeconds = (int)round(lat * 1e-7 * 3600);
        int latDegrees = latSeconds / 3600;
        latSeconds = abs(latSeconds % 3600);
        int latMinutes = latSeconds / 60;
        latSeconds %= 60;
        char latLetter = (lat > 0) ? 'N' : 'S';

        int lonSeconds = (int)round(lon * 1e-7 * 3600);
        int lonDegrees = lonSeconds / 3600;
        lonSeconds = abs(lonSeconds % 3600);
        int lonMinutes = lonSeconds / 60;
        lonSeconds %= 60;
        char lonLetter = (lon > 0) ? 'E' : 'W';

        if (sats)
            sprintf(buf, "%c%02i° %2i'%02i\"   %u sats\n%c%02i° %2i'%02i\"   %d%s", latLetter, abs(latDegrees), latMinutes,
                    latSeconds, sats, lonLetter, abs(lonDegrees), lonMinutes, lonSeconds, altU, units);
        else
            sprintf(buf, "%c%02i° %2i'%02i\"\n%c%02i° %2i'%02i\"   %d%s", latLetter, abs(latDegrees), latMinutes, latSeconds,
                    lonLetter, abs(lonDegrees), lonMinutes, lonSeconds, altU, units);

        lv_label_set_text(objects.home_location_label, buf);

        if (lat != 0 && lon != 0) {
            hasPosition = true;
            myLatitude = lat;
            myLongitude = lon;

            // go through existing node list and update distance
            // TODO: need incremental update!?
            for (auto &it : nodes) {
                if (it.first != ownNode) {
                    int32_t nlat = (long)it.second->LV_OBJ_IDX(node_pos1_idx)->user_data;
                    int32_t nlon = (long)it.second->LV_OBJ_IDX(node_pos2_idx)->user_data;
                    if (nlat != 0 && nlon != 0) {
                        updateDistance(it.first, nlat, nlon);
                    }
                }
            }
            // update own location on both maps (mesh map + our Maps app)
            if (map)
                map->setGpsPosition(lat * 1e-7, lon * 1e-7);
            if (userMap)
                userMap->setGpsPosition(lat * 1e-7, lon * 1e-7);
        }
    } else {
        if (lat != 0 && lon != 0) {
            if (hasPosition) {
                updateDistance(nodeNum, lat, lon);
            }
            addOrUpdateMap(nodeNum, lat, lon);
        }
    }

    if (lat != 0 && lon != 0) {
        char buf[32];
        sprintf(buf, "%.5f %.5f", lat * 1e-7, lon * 1e-7);
        lv_obj_t *panel = nodes[nodeNum];
        lv_label_set_text(panel->LV_OBJ_IDX(node_pos1_idx), buf);
        if (sats)
            sprintf(buf, "%d%s MSL  %u sats", altU, units, sats);
        sprintf(buf, "%d%s MSL", altU, units);
        lv_label_set_text(panel->LV_OBJ_IDX(node_pos2_idx), buf);
        // store lat/lon in user_data, because we need these values later to calculate the distance to us
        panel->LV_OBJ_IDX(node_pos1_idx)->user_data = (void *)lat;
        panel->LV_OBJ_IDX(node_pos2_idx)->user_data = (void *)lon;
        lv_obj_remove_flag(panel->LV_OBJ_IDX(node_pos1_idx), LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(panel->LV_OBJ_IDX(node_pos2_idx), LV_OBJ_FLAG_HIDDEN);
    }

    applyNodesFilter(nodeNum);
}

void TFTView_320x240::updateDistance(uint32_t nodeNum, int32_t lat, int32_t lon)
{
    // if we know our position then calculate (simple) distance to other node in km
    float dx = 71.5 * 1e-7 * (myLongitude - lon);
    float dy = 111.3 * 1e-7 * (myLatitude - lat);
    float dist = sqrt(dx * dx + dy * dy);

    // add distance to user short field
    char buf[32];
    char *userData = (char *)&(nodes[nodeNum]->LV_OBJ_IDX(node_lbs_idx)->user_data);
    buf[0] = userData[0];
    buf[1] = userData[1];
    buf[2] = userData[2];
    buf[3] = userData[3];
    buf[4] = '\n';

    if (db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC) {
        if (dist > 1.0)
            sprintf(&buf[5], "%.1f km ", dist);
        else
            sprintf(&buf[5], "%d m ", (uint32_t)round(dist * 1000));
    } else {
        if (dist > 0.1)
            sprintf(&buf[5], "%.1f mi ", round(dist * 0.621371));
        else
            sprintf(&buf[5], "%d ft ", uint32_t(dist * 3280.84));
    }
    // we used the userShort label to add the distance, so re-arrange a bit the position
    lv_obj_t *userShort = nodes[nodeNum]->LV_OBJ_IDX(node_lbs_idx);
    lv_label_set_text(userShort, buf);
    lv_obj_set_pos(userShort, 30, -1);
}

/**
 * @brief Update battery level and air utilisation
 *
 * @param nodeNum
 * @param bat_level
 * @param voltage
 * @param chUtil
 * @param airUtil
 */
void TFTView_320x240::updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chUtil, float airUtil)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end()) {
        char buf[48];
        if (it->first == ownNode) {
            sprintf(buf, _("Util %0.1f%%  Air %0.1f%%"), chUtil, airUtil);
            lv_label_set_text(it->second->LV_OBJ_IDX(node_sig_idx), buf);

            // update battery percentage and symbol
            if (bat_level != 0 || voltage != 0) {
                uint32_t shown_level = std::min(bat_level, (uint32_t)100);
                sprintf(buf, "%d%%", shown_level);
                bool alert = false;

                BatteryLevel level;
                BatteryLevel::Status status = level.calcStatus(bat_level, voltage);
                switch (status) {
                case BatteryLevel::Plugged:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_plug_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    if (shown_level == 100)
                        buf[0] = '\0';
                    break;
                case BatteryLevel::Charging:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_bolt_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    break;
                case BatteryLevel::Full:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_full_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    break;
                case BatteryLevel::Mid:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_mid_image, LV_PART_MAIN | LV_STATE_DEFAULT);
                    break;
                case BatteryLevel::Low:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_low_image, LV_PART_MAIN | LV_STATE_DEFAULT);
                    break;
                case BatteryLevel::Empty:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_empty_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    break;
                case BatteryLevel::Warn:
                    lv_obj_set_style_bg_image_src(objects.battery_image, &img_battery_empty_warn_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    buf[0] = '\0';
                    alert = true;
                    break;
                default:
                    ILOG_ERROR("unhandled battery level %d", status);
                    break;
                }
                Themes::recolorTopLabel(objects.battery_percentage_label, alert);
                lv_obj_set_style_bg_image_recolor_opa(objects.battery_image, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_label_set_text(objects.battery_percentage_label, buf);

                // mirror the reading into the launcher's top-bar battery %
                launcherBatPct = (int)shown_level;
                launcherBatPlugged = (status == BatteryLevel::Plugged || status == BatteryLevel::Charging);
                updateLauncherBattery();
            }
        }

        if (bat_level != 0 || voltage != 0) {
            bat_level = std::min(bat_level, (uint32_t)100);
            sprintf(buf, "%d%% %0.2fV", bat_level, voltage);
            lv_label_set_text(it->second->LV_OBJ_IDX(node_bat_idx), buf);
            lv_obj_remove_flag(it->second->LV_OBJ_IDX(node_bat_idx), LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TFTView_320x240::updateEnvironmentMetrics(uint32_t nodeNum, const meshtastic_EnvironmentMetrics &metrics)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end()) {
        char buf[50];
        if (db.config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC) {
            if ((int)metrics.relative_humidity > 0) {
                sprintf(buf, "%2.1f°C %d%% %3.1fhPa", metrics.temperature, (int)metrics.relative_humidity,
                        metrics.barometric_pressure);
            } else {
                sprintf(buf, "%2.1f°C %3.1fhPa", metrics.temperature, metrics.barometric_pressure);
            }
        } else {
            if ((int)metrics.relative_humidity > 0) {
                sprintf(buf, "%2.1f°F %d%% %3.1finHg", metrics.temperature * 9 / 5 + 32, (int)metrics.relative_humidity,
                        metrics.barometric_pressure / 33.86f);
            } else {
                sprintf(buf, "%2.1f°F %3.1finHg", metrics.temperature * 9 / 5 + 32, metrics.barometric_pressure / 33.86f);
            }
        }
        lv_label_set_text(it->second->LV_OBJ_IDX(node_tm1_idx), buf);
        lv_obj_remove_flag(it->second->LV_OBJ_IDX(node_tm1_idx), LV_OBJ_FLAG_HIDDEN);

        if (metrics.iaq > 0 && metrics.iaq < 1000) {
            sprintf(buf, "IAQ: %d %.1fV %.1fmA", metrics.iaq, metrics.voltage, metrics.current);
            lv_label_set_text(it->second->LV_OBJ_IDX(node_tm2_idx), buf);
            it->second->LV_OBJ_IDX(node_tm2_idx)->user_data = (void *)(uint32_t)metrics.iaq;
            lv_obj_remove_flag(it->second->LV_OBJ_IDX(node_tm2_idx), LV_OBJ_FLAG_HIDDEN);
        }
        applyNodesFilter(nodeNum);
    }
}

void TFTView_320x240::updateAirQualityMetrics(uint32_t nodeNum, const meshtastic_AirQualityMetrics &metrics)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end() && it->first != ownNode) {
        // TODO
        // char buf[32];
        // sprintf(buf, "%d %d", metrics.particles_03um, metrics.pm100_environmental);
        // lv_label_set_text(it->second->LV_OBJ_IDX(node_tm2_idx), buf);
    }
}

void TFTView_320x240::updatePowerMetrics(uint32_t nodeNum, const meshtastic_PowerMetrics &metrics)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end() && it->first != ownNode) {
        // TODO
        // char buf[32];
        // sprintf(buf, "%0.1fmA %0.2fV", metrics.ch1_current, metrics.ch1_voltage);
        // lv_label_set_text(it->second->LV_OBJ_IDX(node_tm2_idx), buf);
    }
}

/**
 * update signal strength for direct neighbors
 */
void TFTView_320x240::updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr)
{
    if (nodeNum != ownNode) {
        auto it = nodes.find(nodeNum);
        if (it != nodes.end()) {
            char buf[32];
            if (rssi == 0 && snr == 0.0) {
                buf[0] = '\0';
            } else {
                sprintf(buf, "rssi: %d snr: %.1f", rssi, snr);
            }
            lv_label_set_text(it->second->LV_OBJ_IDX(node_sig_idx), buf);
            it->second->LV_OBJ_IDX(node_sig_idx)->user_data = 0;
            lv_obj_remove_flag(it->second->LV_OBJ_IDX(node_sig_idx), LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TFTView_320x240::updateHopsAway(uint32_t nodeNum, uint8_t hopsAway)
{
    if (nodeNum != ownNode) {
        auto it = nodes.find(nodeNum);
        if (it != nodes.end()) {
            char buf[32];
            sprintf(buf, _("hops: %d"), (int)hopsAway);
            lv_label_set_text(it->second->LV_OBJ_IDX(node_sig_idx), buf);
            it->second->LV_OBJ_IDX(node_sig_idx)->user_data = (void *)(unsigned long)hopsAway;
            lv_obj_remove_flag(it->second->LV_OBJ_IDX(node_sig_idx), LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void TFTView_320x240::updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status)
{
    db.connectionStatus = status;
    if (status.has_wifi) {
        if (db.config.network.wifi_enabled || db.config.network.eth_enabled) {
            if (status.wifi.has_status) {
                char buf[20];
                uint32_t ip = status.wifi.status.ip_address;
                sprintf(buf, "%d.%d.%d.%d", ip & 0xff, (ip & 0xff00) >> 8, (ip & 0xff0000) >> 16, (ip & 0xff000000) >> 24);
                lv_label_set_text(objects.home_wlan_label, buf);
                Themes::recolorButton(objects.home_wlan_button, true);
                Themes::recolorText(objects.home_wlan_label, true);
                if (status.wifi.status.is_connected) {
                    lv_obj_set_style_bg_img_src(objects.home_wlan_button, &img_home_wlan_button_image,
                                                LV_PART_MAIN | LV_STATE_DEFAULT);
                } else {
                    lv_obj_set_style_bg_img_src(objects.home_wlan_button, &img_home_wlan_off_image,
                                                LV_PART_MAIN | LV_STATE_DEFAULT);
                }

                if (status.wifi.status.is_mqtt_connected) {
                    Themes::recolorButton(objects.home_mqtt_button, true, 255);
                    Themes::recolorText(objects.home_mqtt_label, true);
                } else {
                    Themes::recolorButton(objects.home_mqtt_button, db.module_config.mqtt.enabled);
                    Themes::recolorText(objects.home_mqtt_label, false);
                }
            }
        } else {
            Themes::recolorButton(objects.home_wlan_button, false);
            Themes::recolorText(objects.home_wlan_label, false);
            if (status.wifi.status.is_mqtt_connected) {
                Themes::recolorButton(objects.home_mqtt_button, true, 255);
                Themes::recolorText(objects.home_mqtt_label, true);
            } else {
                Themes::recolorButton(objects.home_mqtt_button, db.module_config.mqtt.enabled, 100);
                Themes::recolorText(objects.home_mqtt_label, false);
            }
            lv_obj_set_style_bg_img_src(objects.home_wlan_button, &img_home_wlan_off_image, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        lv_obj_add_flag(objects.home_wlan_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.home_wlan_button, LV_OBJ_FLAG_HIDDEN);
    }

    if (status.has_bluetooth) {
        if (db.config.bluetooth.enabled) {
            if (status.bluetooth.is_connected) {
                char buf[20];
                uint32_t mac = ownNode;
                lv_obj_set_style_text_color(objects.home_bluetooth_label, colorLightGray, LV_PART_MAIN | LV_STATE_DEFAULT);
                sprintf(buf, "??:??:%02x:%02x:%02x:%02x", mac & 0xff, (mac & 0xff00) >> 8, (mac & 0xff0000) >> 16,
                        (mac & 0xff000000) >> 24);
                lv_label_set_text(objects.home_bluetooth_label, buf);
                lv_obj_set_style_bg_opa(objects.home_bluetooth_button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_img_src(objects.home_bluetooth_button, &img_home_bluetooth_on_button_image,
                                            LV_PART_MAIN | LV_STATE_DEFAULT);
            } else {
                lv_obj_set_style_text_color(objects.home_bluetooth_label, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_img_src(objects.home_bluetooth_button, &img_home_bluetooth_on_button_image,
                                            LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_img_recolor_opa(objects.home_bluetooth_button, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        } else {
            lv_obj_set_style_text_color(objects.home_bluetooth_label, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_img_src(objects.home_bluetooth_button, &img_home_bluetooth_off_button_image,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_img_recolor_opa(objects.home_bluetooth_button, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        lv_obj_add_flag(objects.home_bluetooth_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.home_bluetooth_button, LV_OBJ_FLAG_HIDDEN);
    }

    if (status.has_ethernet) {
        if (status.ethernet.status.is_connected) {
            char buf[20];
            uint32_t mac = ownNode;
            sprintf(buf, "??:??:%02x:%02x:%02x:%02x", mac & 0xff000000, mac & 0xff0000, mac & 0xff00, mac & 0xff);
            lv_label_set_text(objects.home_ethernet_label, buf);
            lv_obj_set_style_text_color(objects.home_ethernet_label, colorLightGray, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(objects.home_ethernet_button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        } else {
            lv_obj_set_style_bg_img_recolor_opa(objects.home_ethernet_button, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(objects.home_ethernet_label, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        lv_obj_add_flag(objects.home_ethernet_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(objects.home_ethernet_button, LV_OBJ_FLAG_HIDDEN);
    }
}

// ResponseHandler callbacks

void TFTView_320x240::onTextMessageCallback(const ResponseHandler::Request &req, ResponseHandler::EventType evt, int32_t result)
{
    ILOG_DEBUG("onTextMessageCallback: %d %d", evt, result);
    if (evt == ResponseHandler::found) {
        handleTextMessageResponse((unsigned long)req.cookie, req.id, false, result);
    } else if (evt == ResponseHandler::removed) {
        handleTextMessageResponse((unsigned long)req.cookie, req.id, true, result);
    } else {
        ILOG_DEBUG("onTextMessageCallback: timeout!");
    }
}

void TFTView_320x240::onPositionCallback(const ResponseHandler::Request &req, ResponseHandler::EventType evt, int32_t) {}

void TFTView_320x240::onTracerouteCallback(const ResponseHandler::Request &req, ResponseHandler::EventType evt, int32_t) {}

/**
 * handle response from routing
 */
void TFTView_320x240::handleResponse(uint32_t from, const uint32_t id, const meshtastic_Routing &routing,
                                     const meshtastic_MeshPacket &p)
{
    ResponseHandler::Request req{};
    bool ack = false;
    if (from == ownNode) {
        req = requests.findRequest(id);
    } else {
        req = requests.removeRequest(id);
        ack = true;
    }

    if (req.type == ResponseHandler::noRequest) {
        ILOG_WARN("request id 0x%08x not valid (anymore)", id);
    } else {
        ILOG_DEBUG("handleResponse request id 0x%08x", id);
    }
    ILOG_DEBUG("routing tag variant: %d, error: %d", routing.which_variant, routing.error_reason);
    switch (routing.which_variant) {
    case meshtastic_Routing_error_reason_tag: {
        if (routing.error_reason == meshtastic_Routing_Error_NONE) {
            if (req.type == ResponseHandler::TraceRouteRequest) {
                handleTraceRouteResponse(routing);
            } else if (req.type == ResponseHandler::TextMessageRequest) {
                handleTextMessageResponse((unsigned long)req.cookie, id, ack, false);
            } else if (req.type == ResponseHandler::PositionRequest) {
                handlePositionResponse(from, id, p.rx_rssi, p.rx_snr, p.hop_limit == p.hop_start);
            }
        } else if (routing.error_reason == meshtastic_Routing_Error_MAX_RETRANSMIT) {
            ResponseHandler::Request req = requests.removeRequest(id);
            if (req.type == ResponseHandler::TraceRouteRequest) {
                handleTraceRouteResponse(routing);
            } else if (req.type == ResponseHandler::TextMessageRequest) {
                handleTextMessageResponse((unsigned long)req.cookie, id, ack, true);
            }
        } else if (routing.error_reason == meshtastic_Routing_Error_NO_RESPONSE) {
            if (req.type == ResponseHandler::PositionRequest) {
                handlePositionResponse(from, id, p.rx_rssi, p.rx_snr, p.hop_limit == p.hop_start);
            }
        } else if (routing.error_reason == meshtastic_Routing_Error_NO_CHANNEL ||
                   routing.error_reason == meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY) {
            if (req.type == ResponseHandler::TextMessageRequest) {
                handleTextMessageResponse((unsigned long)req.cookie, id, ack, true);
                // we probably have a wrong key; mark it as bad and don't use in future
                if ((unsigned long)nodes[from]->LV_OBJ_IDX(node_bat_idx)->user_data == 1) {
                    ILOG_DEBUG("public key mismatch");
                    nodes[from]->LV_OBJ_IDX(node_bat_idx)->user_data = (void *)2;
                    lv_obj_set_style_border_color(nodes[from]->LV_OBJ_IDX(node_img_idx), colorRed,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_obj_set_style_bg_image_src(objects.top_messages_node_image, &img_lock_slash_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        } else {
            ILOG_DEBUG("got Routing_Error %d", routing.error_reason);
        }
        break;
    }
    case meshtastic_Routing_route_request_tag: {
        ILOG_ERROR("got meshtastic_Routing_route_request_tag");
        break;
    }
    case meshtastic_Routing_route_reply_tag: {
        ILOG_DEBUG("got meshtastic_Routing_route_reply_tag");
        handleResponse(from, id, routing.route_reply);
        break;
    }
    default:
        ILOG_ERROR("unhandled meshtastic_Routing tag");
        break;
    }
}

/**
 * Signal scanner
 */
void TFTView_320x240::scanSignal(uint32_t scanNo)
{
    if (scans == 1 && spinnerButton) {
        lv_label_set_text(objects.signal_scanner_start_label, _("Start"));
        removeSpinner();
    } else {
        uint32_t requestId;
        uint32_t to = currentNode;
        uint8_t ch = (uint8_t)(unsigned long)currentPanel->user_data;
        requestId = requests.addRequest(to, ResponseHandler::PositionRequest, (void *)to);
        controller->requestPosition(to, ch, requestId);
        objects.signal_scanner_panel->user_data = (void *)requestId;
    }
}

void TFTView_320x240::handlePositionResponse(uint32_t from, uint32_t request_id, int32_t rx_rssi, float rx_snr, bool isNeighbor)
{
    if (request_id == (unsigned long)objects.signal_scanner_panel->user_data) {
        requests.removeRequest(request_id);

        if (from == currentNode && isNeighbor) {
            char buf[20];
            sprintf(buf, "SNR\n%.1f", rx_snr);
            lv_label_set_text(objects.signal_scanner_snr_label, buf);
            sprintf(buf, "RSSI\n%d", rx_rssi);
            lv_label_set_text(objects.signal_scanner_rssi_label, buf);
            lv_slider_set_value(objects.snr_slider, rx_snr, LV_ANIM_ON);
            lv_slider_set_value(objects.rssi_slider, rx_rssi, LV_ANIM_ON);
            sprintf(buf, "%d%%", signalStrength2Percent(rx_rssi, rx_snr));
            lv_label_set_text(objects.signal_scanner_start_label, buf);
        }
    } else {
        ILOG_DEBUG("handlePositionResponse: drop reply with not matching request 0x%08x", request_id);
    }
}

/**
 * Trace Route: handle  ack or timeout
 */
void TFTView_320x240::handleTraceRouteResponse(const meshtastic_Routing &routing)
{
    ILOG_DEBUG("handleTraceRouteResponse: route has %d hops", routing.route_reply.route_count);
    if (routing.error_reason != meshtastic_Routing_Error_NONE) {
        lv_label_set_text(objects.trace_route_start_label, _("Start"));
        removeSpinner();
    } else {
        // we got a first ACK to our route request
        if (spinnerButton) {
            lv_obj_set_style_outline_color(objects.trace_route_start_button, lv_color_hex(0xDBD251),
                                           LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

void TFTView_320x240::handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route)
{
    ILOG_DEBUG("handleResponse: trace route has %d / %d hops", route.route_count, route.route_back_count);
    lv_obj_add_flag(objects.start_button_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(objects.hop_routes_panel, LV_OBJ_FLAG_HIDDEN);

    if (id && requests.findRequest(id).type == ResponseHandler::TraceRouteRequest) {
        requests.removeRequest(id);
    }

    for (int i = route.route_count; i > 0; i--) {
        addNodeToTraceRoute(route.route[i - 1], objects.route_towards_panel);
    }

    for (int i = 0; i < route.route_back_count; i++) {
        addNodeToTraceRoute(route.route_back[i], objects.route_back_panel);
    }

    // route contains only intermediate nodes, so add our node
    addNodeToTraceRoute(ownNode, objects.trace_route_panel);
}

void TFTView_320x240::addNodeToTraceRoute(uint32_t nodeNum, lv_obj_t *panel)
{
    // check if node exists, and get its panel
    lv_obj_t *nodePanel = nullptr;
    auto it = nodes.find(nodeNum);
    if (it != nodes.end()) {
        nodePanel = it->second;
    }
    lv_obj_t *btn = lv_btn_create(panel);
    // objects.trace_route_to_button = btn;
    lv_obj_set_pos(btn, 0, 0);
    lv_obj_set_size(btn, LV_PCT(100), 38);
    add_style_settings_button_style(btn);
    lv_obj_set_style_align(btn, LV_ALIGN_TOP_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
    {
        {
            lv_obj_t *img = lv_img_create(btn);
            if (nodePanel) {
                setNodeImage(nodeNum, (MeshtasticView::eRole)(unsigned long)nodePanel->LV_OBJ_IDX(node_img_idx)->user_data, false,
                             img);
            } else {
                setNodeImage(0, eRole::unknown, false, img);
            }
            lv_obj_set_pos(img, -5, 3);
            lv_obj_set_size(img, 32, 32);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(img, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_image_recolor_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_align(img, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(img, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // TraceRouteToButtonLabel
            lv_obj_t *label = lv_label_create(btn);
            lv_obj_set_pos(label, 35, 10);
            lv_obj_set_size(label, LV_PCT(80), LV_SIZE_CONTENT);
            lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL);
            if (nodePanel) {
                if (nodeNum != ownNode) {
                    lv_obj_add_event_cb(btn, ui_event_trace_route_node, LV_EVENT_CLICKED, nodePanel);
                    lv_label_set_text(label, lv_label_get_text(nodePanel->LV_OBJ_IDX(node_lbs_idx)));
                    if (strlen(lv_label_get_text(label)) >= 5)
                        lv_obj_set_pos(label, 35, -1);
                } else {
                    lv_label_set_text(label, lv_label_get_text(nodePanel->LV_OBJ_IDX(node_lbl_idx)));
                }
            } else {
                char buf[20];
                if (nodeNum != UINT32_MAX) {
                    lv_snprintf(buf, 16, "!%08x", nodeNum);
                    lv_label_set_text(label, buf);
                } else
                    lv_label_set_text(label, _("unknown"));
            }
            lv_obj_set_style_align(label, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

/**
 * @brief purge oldest node from node list (and all its memory)
 * @param nodeNum node that is being added and already contained in nodes[], so don't remove it!
 */
void TFTView_320x240::purgeNode(uint32_t nodeNum)
{
    if (nodeCount <= 1)
        return;

    lv_obj_t **children = objects.nodes_panel->spec_attr->children;
    int last = objects.nodes_panel->spec_attr->child_cnt - 1;
    int i = last;

#ifndef ALWAYS_PURGE_OLDEST_NODE
    time_t curr_time;
#ifdef ARCH_PORTDUINO
    time(&curr_time);
#else
    curr_time = actTime;
#endif
    // prefer purging older unknown nodes first (but not the brand new ones)
    while ((eRole)(long)(children[i]->LV_OBJ_IDX(node_img_idx)->user_data) != eRole::unknown ||
           curr_time < (time_t)(children[i]->LV_OBJ_IDX(node_lh_idx)->user_data) + 120 ||
           (unsigned long)(children[i]->LV_OBJ_IDX(node_lbl_idx)->user_data) == nodeNum ||
           chats.find((unsigned long)(children[i]->LV_OBJ_IDX(node_lbl_idx)->user_data)) != chats.end()) {
        if (i < (last + 1) / 5) { // keep 80% named nodes and 20% unknown (not fresh) nodes
            i = last;
            break;
        }
        i--;
    }
#endif
    lv_obj_t *p = children[i];
    uint32_t oldest = (unsigned long)(p->LV_OBJ_IDX(node_lbl_idx)->user_data);
    uint32_t lastHeard = (unsigned long)p->LV_OBJ_IDX(node_lh_idx)->user_data;
    if (lastHeard > 0 && (curtime - lastHeard <= secs_until_offline))
        nodesOnline--;

    ILOG_INFO("removing oldest node 0x%08x", oldest);
    lv_obj_delete(p);
    {
        auto it = messages.find(oldest);
        if (it != messages.end()) {
            lv_obj_delete(it->second);
            messages.erase(oldest);
        }
    }

    {
        auto it = chats.find(oldest);
        if (it != chats.end()) {
            lv_obj_delete(it->second);
            chats.erase(oldest);
            updateActiveChats();
        }
    }
    removeFromMap(oldest);
    nodes.erase(oldest);
    nodeCount--;
    nodesChanged = true; // flag to force re-apply node filter
}

/**
 * @brief apply enabled filters and highlight node
 *
 * @param nodeNum
 * @param reset : set true when filter has changed (to recalculate number of filtered nodes)
 * @return true
 * @return false
 */
bool TFTView_320x240::applyNodesFilter(uint32_t nodeNum, bool reset)
{
    lv_obj_t *panel = nodes[nodeNum];
    bool hide = false;
    if (nodeNum != ownNode /* && filter.active*/) { // TODO
        if (lv_obj_has_state(objects.nodes_filter_unknown_switch, LV_STATE_CHECKED)) {
            if (lv_img_get_src(panel->LV_OBJ_IDX(node_img_idx)) == &img_circle_question_image) {
                hide = true;
            }
        }
        if (lv_obj_has_state(objects.nodes_filter_offline_switch, LV_STATE_CHECKED)) {
            time_t lastHeard = (time_t)panel->LV_OBJ_IDX(node_lh_idx)->user_data;
            if (lastHeard == 0 || curtime - lastHeard > secs_until_offline)
                hide = true;
        }
        if (lv_obj_has_state(objects.nodes_filter_public_key_switch, LV_STATE_CHECKED)) {
            bool hasKey = (unsigned long)panel->LV_OBJ_IDX(node_bat_idx)->user_data == 1;
            if (!hasKey)
                hide = true;
        }
        if (lv_dropdown_get_selected(objects.nodes_filter_channel_dropdown) != 0) {
            int selected = lv_dropdown_get_selected(objects.nodes_filter_channel_dropdown);
            if (selected != 0) {
                uint8_t ch = (uint8_t)(unsigned long)panel->user_data;
                if (selected - 1 != ch)
                    hide = true;
            }
        }
        if (lv_dropdown_get_selected(objects.nodes_filter_hops_dropdown) != 0) {
            int32_t hopsAway = (signed long)panel->LV_OBJ_IDX(node_sig_idx)->user_data;
            int selected = lv_dropdown_get_selected(objects.nodes_filter_hops_dropdown) - 7;
            if (hopsAway < 0)
                hide = true;
            else if (selected <= 0) {
                if (hopsAway > -selected)
                    hide = true;
            } else {
                if (hopsAway < selected)
                    hide = true;
            }
        }
#if 0
        if (lv_obj_has_state(objects.nodes_filter_mqtt_switch, LV_STATE_CHECKED)) {
            bool viaMqtt = false; // TODO (unsigned long)panel->LV_OBJ_IDX(node_sig_idx)->user_data;
            if (viaMqtt)
                hide = true;
        }
#endif
        if (lv_obj_has_state(objects.nodes_filter_position_switch, LV_STATE_CHECKED)) {
            if (lv_label_get_text(panel->LV_OBJ_IDX(node_pos1_idx))[0] == '\0')
                hide = true;
        }
        const char *name = lv_textarea_get_text(objects.nodes_filter_name_area);
        if (name[0] != '\0') {
            if (name[0] != '!') { // use '!' char to negate search result
                if (!strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbl_idx)), name) &&
                    !strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbs_idx)), name)) {
                    hide = true;
                }
            } else {
                if (strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbl_idx)), &name[1]) ||
                    strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbs_idx)), &name[1])) {
                    hide = true;
                }
            }
        }
    }
    if (hide) {
        if (reset || !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
            nodesFiltered++;
        }
    } else {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }

    // hide node location if filtered
    if (map)
        map->update(nodeNum, hide);

    bool highlight = false;
    if (true /*highlight.active*/) { // TODO
        if (lv_obj_has_state(objects.nodes_hl_active_chat_switch, LV_STATE_CHECKED)) {
            auto it = chats.find(nodeNum);
            if (it != nodes.end()) {
                lv_obj_set_style_border_color(panel, colorOrange, LV_PART_MAIN | LV_STATE_DEFAULT);
                highlight = true;
            }
        }
        if (lv_obj_has_state(objects.nodes_hl_position_switch, LV_STATE_CHECKED)) {
            if (lv_label_get_text(panel->LV_OBJ_IDX(node_pos1_idx))[0] != '\0') {
                lv_obj_set_style_border_color(panel, colorBlueGreen, LV_PART_MAIN | LV_STATE_DEFAULT);
                highlight = true;
            }
        }
        if (lv_obj_has_state(objects.nodes_hl_telemetry_switch, LV_STATE_CHECKED)) {
            if (lv_label_get_text(panel->LV_OBJ_IDX(node_tm1_idx))[0] != '\0') {
                lv_obj_set_style_border_color(panel, colorBlue, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
                highlight = true;
            }
        }
        if (lv_obj_has_state(objects.nodes_hliaq_switch, LV_STATE_CHECKED)) {
            if (lv_label_get_text(panel->LV_OBJ_IDX(node_tm2_idx))[0] != '\0') {
                uint32_t iaq = (unsigned long)panel->LV_OBJ_IDX(node_tm2_idx)->user_data;
                // IAQ color code
                lv_color_t fg, bg;
                if (iaq <= 50) {
                    fg = lv_color_hex(0x00000000);
                    bg = lv_color_hex(0x000ce810);
                } else if (iaq <= 100) {
                    fg = lv_color_hex(0x00000000);
                    bg = lv_color_hex(0x00faf646);
                } else if (iaq <= 150) {
                    fg = lv_color_hex(0x00000000);
                    bg = lv_color_hex(0x00f98204);
                } else if (iaq <= 200) {
                    fg = lv_color_hex(0x00000000);
                    bg = lv_color_hex(0x00e42104);
                } else if (iaq <= 300) {
                    fg = lv_color_hex(0xffffffff);
                    bg = lv_color_hex(0x009b2970);
                } else {
                    fg = lv_color_hex(0xffffffff);
                    bg = lv_color_hex(0x001d1414);
                }
                lv_obj_set_style_text_color(panel->LV_OBJ_IDX(node_tm2_idx), fg, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_color(panel->LV_OBJ_IDX(node_tm2_idx), bg, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(panel->LV_OBJ_IDX(node_tm2_idx), 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_color(panel, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
                highlight = true;
            }
        }
        const char *name = lv_textarea_get_text(objects.nodes_hl_name_area);
        if (name[0] != '\0') {
            if (strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbl_idx)), name) ||
                strcasestr(lv_label_get_text(panel->LV_OBJ_IDX(node_lbs_idx)), name)) {
                lv_obj_set_style_border_color(panel, colorMesh, LV_PART_MAIN | LV_STATE_DEFAULT);
                highlight = true;
            }
        }
    }
    if (!highlight) {
        lv_obj_set_style_border_color(panel, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return hide; // TODO || filter.active;
}

void TFTView_320x240::messageAlert(const char *alert, bool show)
{
    lv_label_set_text(objects.alert_label, alert);
    if (show)
        lv_obj_clear_flag(objects.alert_panel, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(objects.alert_panel, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief mark the sent message as either heard or acknowledged or failed
 *
 * @param channelOrNode
 * @param id
 * @param ack
 */
void TFTView_320x240::handleTextMessageResponse(uint32_t channelOrNode, const uint32_t id, bool ack, bool err)
{
    lv_obj_t *msgContainer;
    if (channelOrNode < c_max_channels) {
        msgContainer = channelGroup[(uint8_t)channelOrNode];
        ack = true; // treat messages sent to group channel same as ack
    } else {
        msgContainer = messages[channelOrNode];
    }
    if (!msgContainer) {
        ILOG_WARN("received unexpected response nodeNum/channel 0x%08x for request id 0x%08x", channelOrNode, id);
        return;
    }
    // go through all hiddenPanels and search for requestId
    uint16_t i = msgContainer->spec_attr->child_cnt;
    while (i-- > 0) {
        lv_obj_t *panel = msgContainer->spec_attr->children[i];
        uint32_t requestId = (unsigned long)panel->user_data;
        if (requestId == id) {
            // now give the textlabel border another color
            lv_obj_t *textLabel = panel->spec_attr->children[0];
            lv_obj_set_style_border_color(textLabel,
                                          err   ? colorRed
                                          : ack ? colorBlueGreen
                                                : colorYellow,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);

            // store message
            break;
        }
    }
}

void TFTView_320x240::packetReceived(const meshtastic_MeshPacket &p)
{
    MeshtasticView::packetReceived(p);

    // try update time from packet
    if (!VALID_TIME(actTime) && VALID_TIME(p.rx_time))
        updateTime(p.rx_time);

    if (detectorRunning) {
        packetDetected(p);
    }
    if (packetLogEnabled) {
        writePacketLog(p);
    }
    if (p.from != ownNode) {
        updateSignalStrength(p.rx_rssi, p.rx_snr);
    }
    updateStatistics(p);
}

void TFTView_320x240::notifyConnected(const char *info)
{
    if (state == MeshtasticView::eBooting) {
        updateBootMessage(info);
    } else {
        if (state == MeshtasticView::eDisconnected) {
            messageAlert(_("Connected!"), true);
            // force re-sync with node
            THIS->controller->setConfigRequested(true);
        }
        state = MeshtasticView::eRunning;
    }
}

void TFTView_320x240::notifyDisconnected(const char *info)
{
    if (state == MeshtasticView::eBooting) {
        updateBootMessage(info);
    } else {
        if (state == MeshtasticView::eRunning) {
            messageAlert(_("Disconnected!"), true);
        }
        state = MeshtasticView::eDisconnected;
    }
}

void TFTView_320x240::notifyResync(bool show)
{
    if (controller->isStandalone()) {
        if (show)
            notifyReboot(true);
    } else {
        messageAlert(_("Resync ..."), show);
        if (!show) {
            lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        }
    }
}

void TFTView_320x240::notifyReboot(bool show)
{
    messageAlert(_("Rebooting ..."), show);
    if (controller->isStandalone()) {
        lv_timer_create(timer_event_reboot, 8000, NULL);
    }
}

void TFTView_320x240::notifyShutdown(void)
{
    messageAlert(_("Shutting down ..."), true);
}

void TFTView_320x240::blankScreen(bool enable)
{
    ILOG_DEBUG("%s screen (%s)", enable ? "blank" : "unblank", screenLocked ? "locked" : "timeout");
    if (enable)
        lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 1000, 0, false);
    else {
        if (launcher_screen) // return to our custom launcher home, not MUI's main_screen
            lv_screen_load_anim(launcher_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        else if (objects.main_screen)
            lv_screen_load_anim(objects.main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        else
            lv_screen_load_anim(objects.boot_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    }
}

void TFTView_320x240::screenSaving(bool enabled)
{
    if (enabled) {
        // overlay main screen with blank screen to prevent accidentally pressing buttons
        lv_screen_load_anim(objects.blank_screen, LV_SCR_LOAD_ANIM_FADE_OUT, 0, 0, false);
        lv_group_focus_obj(objects.blank_screen_button);
        screenLocked = true;
        screenUnlockRequest = false;
    } else {
        if (THIS->db.uiConfig.screen_lock) {
            ILOG_DEBUG("showing lock screen");
            lv_screen_load_anim(objects.lock_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        } else if (launcher_screen || objects.main_screen) {
            ILOG_DEBUG("showing launcher screen");
            lv_screen_load_anim(launcher_screen ? launcher_screen : objects.main_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                                false);
            if (THIS->activeSettings != eNone) {
                lv_event_t e = {.code = LV_EVENT_CLICKED};
                ui_event_cancel(&e);
            }
            screenLocked = false;
        } else {
            ILOG_DEBUG("showing boot screen");
            lv_screen_load_anim(objects.boot_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            screenLocked = false;
        }
    }
}

bool TFTView_320x240::isScreenLocked(void)
{
    return screenLocked && !screenUnlockRequest;
}

void TFTView_320x240::updateChannelConfig(const meshtastic_Channel &ch)
{
    static lv_obj_t *btn[c_max_channels] = {objects.channel_button0, objects.channel_button1, objects.channel_button2,
                                            objects.channel_button3, objects.channel_button4, objects.channel_button5,
                                            objects.channel_button6, objects.channel_button7};
    db.channel[ch.index] = ch;

    if (ch.role != meshtastic_Channel_Role_DISABLED) {
        setChannelName(ch);

        lv_obj_set_width(btn[ch.index], lv_pct(80));
        lv_obj_set_style_pad_left(btn[ch.index], 8, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *lockImage = NULL;
        if (lv_obj_get_child_cnt(btn[ch.index]) == 1)
            lockImage = lv_img_create(btn[ch.index]);
        else
            lockImage = lv_obj_get_child(btn[ch.index], 1);

        uint32_t recolor = 0;

        if (memcmp(ch.settings.psk.bytes, "\001\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000", 16) == 0) {
            lv_image_set_src(lockImage, &img_groups_key_image);
            recolor = 0xF2E459; // yellow
        } else if (memcmp(ch.settings.psk.bytes, "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000", 16) == 0) {
            lv_image_set_src(lockImage, &img_groups_unlock_image);
            recolor = 0xF72B2B; // reddish
        } else {
            lv_image_set_src(lockImage, &img_groups_lock_image);
            recolor = 0x1EC174; // green
        }
        lv_obj_set_width(lockImage, LV_SIZE_CONTENT);  /// 1
        lv_obj_set_height(lockImage, LV_SIZE_CONTENT); /// 1
        lv_obj_set_align(lockImage, LV_ALIGN_LEFT_MID);
        lv_obj_add_flag(lockImage, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
        lv_obj_clear_flag(lockImage, LV_OBJ_FLAG_SCROLLABLE); /// Flags
        lv_obj_set_style_img_recolor(lockImage, lv_color_hex(recolor), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(lockImage, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *bellImage = NULL;
        if (lv_obj_get_child_cnt(btn[ch.index]) < 3)
            bellImage = lv_img_create(btn[ch.index]);
        else
            bellImage = lv_obj_get_child(btn[ch.index], 2);
        lv_obj_set_width(bellImage, LV_SIZE_CONTENT);  /// 1
        lv_obj_set_height(bellImage, LV_SIZE_CONTENT); /// 1
        lv_obj_set_align(bellImage, LV_ALIGN_RIGHT_MID);
        lv_obj_add_flag(bellImage, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
        lv_obj_clear_flag(bellImage, LV_OBJ_FLAG_SCROLLABLE); /// Flags
        lv_obj_set_style_img_recolor_opa(bellImage, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        updateGroupChannel(ch.index);
    } else {
        // display smaller button with just the channel number
        char buf[10];
        lv_snprintf(buf, sizeof(buf), "%d", ch.index);
        lv_label_set_text(channel[ch.index], buf);
        lv_obj_set_width(btn[ch.index], lv_pct(30));

        if (lv_obj_get_child_cnt(btn[ch.index]) == 2) {
            lv_obj_delete(lv_obj_get_child(btn[ch.index], 1));
        }
    }
}

// redraw bell icons and color
void TFTView_320x240::updateGroupChannel(uint8_t chId)
{
    static lv_obj_t *btn[c_max_channels] = {objects.channel_button0, objects.channel_button1, objects.channel_button2,
                                            objects.channel_button3, objects.channel_button4, objects.channel_button5,
                                            objects.channel_button6, objects.channel_button7};

    lv_obj_t *bellImage = lv_obj_get_child(btn[chId], 2);
    if (db.channel[chId].settings.module_settings.is_muted) {
        lv_obj_set_style_img_recolor(bellImage, lv_color_hex(0xffab0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_image_set_src(bellImage, &img_groups_bell_slash_image);
    } else {
        Themes::recolorImage(bellImage, true);
        lv_image_set_src(bellImage, &img_groups_bell_image);
    }
}

void TFTView_320x240::updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg)
{
    db.config.device = cfg;
    db.config.has_device = true;

    char buf1[30], buf2[40];
    lv_dropdown_set_selected(objects.settings_device_role_dropdown, role2val(cfg.role));
    lv_dropdown_get_selected_str(objects.settings_device_role_dropdown, buf1, sizeof(buf1));
    lv_snprintf(buf2, sizeof(buf2), _("Device Role: %s"), buf1);
    lv_label_set_text(objects.basic_settings_role_label, buf2);
}

void TFTView_320x240::updatePositionConfig(const meshtastic_Config_PositionConfig &cfg)
{
    db.config.position = cfg;
    db.config.has_position = true;
    if (cfg.gps_mode != meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT) {
        if (cfg.fixed_position && db.uiConfig.map_data.has_home) {
            updatePosition(ownNode, db.uiConfig.map_data.home.latitude, db.uiConfig.map_data.home.longitude, 0, 0, 0);
        }
        // grey out text to indicate it's a fixed position vs. actual GPS position
        Themes::recolorText(objects.home_location_label, !cfg.fixed_position);
    }
    Themes::recolorButton(objects.home_location_button, cfg.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED);
}

void TFTView_320x240::updatePowerConfig(const meshtastic_Config_PowerConfig &cfg)
{
    db.config.power = cfg;
    db.config.has_power = true;
}

void TFTView_320x240::updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg)
{
    db.config.network = cfg;
    db.config.has_network = true;

    char buf[40];
    lv_snprintf(buf, sizeof(buf), _("WiFi: %s"), cfg.wifi_ssid[0] ? cfg.wifi_ssid : _("<not set>"));
    lv_label_set_text(objects.basic_settings_wifi_label, buf);
}

void TFTView_320x240::updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg)
{
    db.config.display = cfg;
    db.config.has_display = true;
    if (!controller->isStandalone() && cfg.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
        meshtastic_Config_DisplayConfig &display = db.config.display;
        display.displaymode = meshtastic_Config_DisplayConfig_DisplayMode_COLOR;
        THIS->controller->sendConfig(meshtastic_Config_DisplayConfig{display}, THIS->ownNode);
    }
}

void TFTView_320x240::updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg)
{
    db.config.lora = cfg;
    db.config.has_lora = true;

    if (cfg.use_preset) {
        // This must be run before displaying LoRa frequency as channel of 0 ("calculate from hash") leads to an integer underflow
        if (!db.config.lora.channel_num) {
            db.config.lora.channel_num = LoRaPresets::getDefaultSlot(db.config.lora.region, THIS->db.config.lora.modem_preset,
                                                                     THIS->db.channel[0].settings.name);
        }
        char buf1[20], buf2[32];
        lv_dropdown_set_selected(objects.settings_modem_preset_dropdown, preset2val(cfg.modem_preset));
        lv_dropdown_get_selected_str(objects.settings_modem_preset_dropdown, buf1, sizeof(buf1));
        lv_snprintf(buf2, sizeof(buf2), _("Modem Preset: %s"), buf1);
        lv_label_set_text(objects.basic_settings_modem_preset_label, buf2);

        uint32_t numChannels = LoRaPresets::getNumChannels(cfg.region, cfg.modem_preset);
        lv_slider_set_range(objects.frequency_slot_slider, 1, numChannels);
        lv_slider_set_value(objects.frequency_slot_slider, db.config.lora.channel_num, LV_ANIM_OFF);
    } else {
        lv_label_set_text(objects.basic_settings_modem_preset_label, _("Modem Preset: custom"));
    }

    char region[30];
    lv_snprintf(region, sizeof(region), _("Region: %s"), LoRaPresets::loRaRegionToString(cfg.region));
    lv_label_set_text(objects.basic_settings_region_label, region);

    showLoRaFrequency(db.config.lora);

    if (db.config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        // update channel names again now that region is known
        for (int i = 0; i < c_max_channels; i++) {
            if (db.channel[i].has_settings && db.channel[i].role != meshtastic_Channel_Role_DISABLED) {
                setChannelName(db.channel[i]);
            }
        }
    } else {
        requestSetup();
    }
}

void TFTView_320x240::showLoRaFrequency(const meshtastic_Config_LoRaConfig &cfg)
{
    char loraFreq[48];
    if (!cfg.region) {
        strcpy(loraFreq, _("region unset"));
    } else if (cfg.use_preset) {
        float frequency = LoRaPresets::getRadioFreq(cfg.region, cfg.modem_preset, cfg.channel_num) + cfg.frequency_offset;
        sprintf(loraFreq, "LoRa %g MHz\n[%s kHz]", frequency, LoRaPresets::getBandwidthString(cfg.modem_preset));
        lv_obj_remove_state(objects.basic_settings_modem_preset_button, LV_STATE_DISABLED);
    } else {
        float frequency = cfg.override_frequency + cfg.frequency_offset;
        sprintf(loraFreq, "LoRa %g MHz\n[%d kHz]", frequency, cfg.bandwidth);
        lv_obj_add_state(objects.basic_settings_modem_preset_button, LV_STATE_DISABLED);
    }

    lv_label_set_text(objects.home_lora_label, loraFreq);
    Themes::recolorButton(objects.home_lora_button, cfg.tx_enabled);
    Themes::recolorText(objects.home_lora_label, cfg.tx_enabled);
    if (!cfg.tx_enabled) {
        lv_obj_clear_flag(objects.top_lora_tx_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(objects.top_lora_tx_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void TFTView_320x240::setBellText(bool banner, bool sound)
{
    if (banner && sound) {
        lv_label_set_text(objects.home_bell_label, _("Banner & Sound"));
    } else if (banner) {
        lv_label_set_text(objects.home_bell_label, _("Banner only"));
    } else if (sound) {
        lv_label_set_text(objects.home_bell_label, _("Sound only"));
    } else {
        lv_label_set_text(objects.home_bell_label, _("silent"));
    }

    char buf[40];
    lv_snprintf(buf, sizeof(buf), _("Message Alert: %s"),
                db.module_config.external_notification.alert_message_buzzer
                    ? (!sound ? _("silent") : ringtone[db.uiConfig.ring_tone_id].name)
                    : "off");
    lv_label_set_text(objects.basic_settings_alert_label, buf);

    Themes::recolorButton(objects.home_bell_button, banner || sound);
    Themes::recolorText(objects.home_bell_label, banner || sound);
}

/**
 * auto set primary(secondary) channel name (based on region)
 */
void TFTView_320x240::setChannelName(const meshtastic_Channel &ch)
{
    char buf[40];
    if (ch.role == meshtastic_Channel_Role_PRIMARY) {
        sprintf(buf, _("Channel: %s"),
                strlen(ch.settings.name) ? ch.settings.name
                : db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET
                    ? ("<unset>")
                    : LoRaPresets::modemPresetToString(db.config.lora.modem_preset));
        lv_label_set_text(objects.basic_settings_channel_label, buf);

        sprintf(buf, "*%s",
                strlen(ch.settings.name) ? ch.settings.name
                : db.config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET
                    ? ("<unset>")
                    : LoRaPresets::modemPresetToString(db.config.lora.modem_preset));
    } else {
        if (ch.settings.name[0] == '\0' && ch.settings.psk.size == 1 && ch.settings.psk.bytes[0] == 0x01) {
            sprintf(buf, "%s", LoRaPresets::modemPresetToString(db.config.lora.modem_preset));
        } else {
            strcpy(buf, ch.settings.name);
        }
    }

    lv_label_set_text(channel[ch.index], buf);

    // rename chat
    auto it = chats.find(ch.index);
    if (it != chats.end()) {
        char buf2[64];
        sprintf(buf2, "%d: %s", (int)ch.index, buf);
        lv_label_set_text(it->second->spec_attr->children[0], buf2);
    }
}

void TFTView_320x240::backup(uint32_t option)
{
#if defined(HAS_SDCARD) || defined(HAS_SD_MMC) || defined(ARCH_PORTDUINO)
    meshtastic_Config_SecurityConfig_public_key_t &pubkey = db.config.security.public_key;
    meshtastic_Config_SecurityConfig_private_key_t &privkey = db.config.security.private_key;

    std::stringstream path;
    path << "/keys/" << std::hex << std::setw(8) << std::setfill('0') << ownNode << ".yml";
#if defined(ARCH_PORTDUINO) || defined(HAS_SD_MMC)
    SDFs.mkdir("/keys");
    File sd = SDFs.open(path.str().c_str(), FILE_WRITE);
#else
    SDFs.mkdir("/keys");
    FsFile sd = SDFs.open(path.str().c_str(), O_RDWR | O_CREAT);
#endif
    if (sd) {
        sd.println("config:");
        sd.println("  security:");
        sd.print("      privateKey: base64:");
        sd.println(pskToBase64(privkey.bytes, privkey.size).c_str());
        sd.print("      publicKey: base64:");
        sd.println(pskToBase64(pubkey.bytes, pubkey.size).c_str());
        ILOG_INFO("backup pub/priv keys done.");
    } else {
        ILOG_ERROR("open file %s for backup failed", path.str().c_str());
        messageAlert(_("Failed to write keys!"), true);
    }
    sd.close();
#endif
}

void TFTView_320x240::restore(uint32_t option)
{
#if defined(HAS_SDCARD) || defined(HAS_SD_MMC) || defined(ARCH_PORTDUINO)
    meshtastic_Config_SecurityConfig_public_key_t &pubkey = db.config.security.public_key;
    meshtastic_Config_SecurityConfig_private_key_t &privkey = db.config.security.private_key;

    std::stringstream path;
    path << "/keys/" << std::hex << std::setw(8) << std::setfill('0') << ownNode << ".yml";

#if defined(ARCH_PORTDUINO) || defined(HAS_SD_MMC)
    File sd = SDFs.open(path.str().c_str(), FILE_READ);
#else
    FsFile sd = SDFs.open(path.str().c_str(), O_RDONLY);
#endif
    if (sd) {
        // TODO: improve parsing file contents
        sd.readStringUntil('\n');                  // config:
        sd.readStringUntil('\n');                  // security:
        String privKey = sd.readStringUntil('\n'); // privateKey: base64:
        String pubKey = sd.readStringUntil('\n');  // publicKey: base64:
        if (privKey.indexOf("privateKey:") > 0 && pubKey.indexOf("publicKey:") > 0) {
            String b64priv = privKey.substring(privKey.lastIndexOf(":") + 1);
            String b64pub = pubKey.substring(pubKey.lastIndexOf(":") + 1);
            b64priv.trim();
            b64pub.trim();
            if (base64ToPsk(b64priv.c_str(), privkey.bytes, privkey.size) &&
                base64ToPsk(b64pub.c_str(), pubkey.bytes, pubkey.size) &&
                controller->sendConfig(meshtastic_Config_SecurityConfig{db.config.security})) {
                ILOG_INFO("restore pub/priv keys sent to radio");
            } else {
                ILOG_ERROR("decoding keys failed");
                messageAlert(_("Failed to restore keys!"), true);
            }
        } else {
            ILOG_ERROR("file %s contents don't match backup", path.str().c_str());
            messageAlert(_("Failed to parse keys!"), true);
        }
    } else {
        ILOG_ERROR("open file %s failed", path.str().c_str());
        messageAlert(_("Failed to retrieve keys!"), true);
    }
    sd.close();
#endif
}

/**
 * @brief write local time stamp into buffer
 *        if date is not current also add day/month
 *        Note: time string ends with linefeed
 *
 * @param buf allocated buffer
 * @param datetime date/time to write
 * @param update update with actual time, otherwise using time from parameter 'time'
 * @return length of time string
 */
uint32_t TFTView_320x240::timestamp(char *buf, uint32_t datetime, bool update)
{
    time_t local = datetime;
    if (update) {
#ifdef ARCH_PORTDUINO
        time(&local);
#else
        if (VALID_TIME(actTime))
            local = actTime;
#endif
    }
    if (VALID_TIME(local)) {
        std::tm date_tm{};
        localtime_r(&local, &date_tm);
        if (!update)
            return strftime(buf, 20, "%y/%m/%d %R\n", &date_tm);
        else
            return strftime(buf, 20, "%R\n", &date_tm);
    } else
        return 0;
}

/**
 * calculate percentage value from rssi and snr
 * Note: ranges are based on the axis values of the signal scanner
 */
int32_t TFTView_320x240::signalStrength2Percent(int32_t rx_rssi, float rx_snr)
{
#if defined(USE_SX127x)
    int p_snr = ((std::max<int32_t>(rx_snr, -19.0f) + 19.0f) / 33.0f) * 100.0f; // range -19..14
    int p_rssi = ((std::max<int32_t>(rx_rssi, -145L) + 145) * 100) / 90;        // range -145..-55
#else
    int p_snr = ((std::max<int32_t>(rx_snr, -18.0f) + 18.0f) / 26.0f) * 100.0f; // range -18..8
    int p_rssi = ((std::max<int32_t>(rx_rssi, -125) + 125) * 100) / 100;        // range -125..-25
#endif
    return std::min<int32_t>((p_snr + p_rssi * 2) / 3, 100);
}

void TFTView_320x240::updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id)
{
    db.config.bluetooth = cfg;
    db.config.has_bluetooth = true;

    if (ownNode == 0) {
        ownNode = id;
    }

    if (state <= MeshtasticView::eBootScreenDone && state != MeshtasticView::eWaitingForReboot) {
        enterProgrammingMode();
    }
}

void TFTView_320x240::updateSecurityConfig(const meshtastic_Config_SecurityConfig &cfg)
{
    db.config.security = cfg;
    db.config.has_security = true;

    // display public key in qr code label
    char buf[64];
    lv_snprintf(buf, sizeof(buf), "%s", pskToBase64((uint8_t *)cfg.public_key.bytes, cfg.public_key.size).c_str());
    lv_label_set_text(objects.home_qr_label, buf);
}

void TFTView_320x240::updateSessionKeyConfig(const meshtastic_Config_SessionkeyConfig &cfg)
{
    // TODO
}

/// ---- module updates ----

void TFTView_320x240::updateMQTTModule(const meshtastic_ModuleConfig_MQTTConfig &cfg)
{
    db.module_config.mqtt = cfg;
    db.module_config.has_mqtt = true;

    char buf[32];
    lv_snprintf(buf, sizeof(buf), "%s", db.module_config.mqtt.root);
    lv_label_set_text(objects.home_mqtt_label, buf);

    if (!db.module_config.mqtt.enabled) {
        Themes::recolorButton(objects.home_mqtt_button, false);
        Themes::recolorText(objects.home_mqtt_label, false);
    }
}

void TFTView_320x240::updateExtNotificationModule(const meshtastic_ModuleConfig_ExternalNotificationConfig &cfg)
{
    db.module_config.external_notification = cfg;
    db.module_config.has_external_notification = true;

    char buf[32];
    lv_snprintf(buf, sizeof(buf), _("Message Alert: %s"),
                db.module_config.external_notification.alert_message_buzzer && db.module_config.external_notification.enabled
                    ? _("on")
                    : _("off"));
    lv_label_set_text(objects.basic_settings_alert_label, buf);
}

void TFTView_320x240::updateRingtone(const char rtttl[231])
{
    // retrieving ringtone index for dropdown
    uint16_t rtIndex = 0;
    for (int i = 0; i < numRingtones; i++) {
        if (strncmp(ringtone[i].rtttl, rtttl, 16) == 0) {
            rtIndex = i;
            break;
        }
    }
    if (rtIndex != 0)
        db.uiConfig.ring_tone_id = rtIndex;
    if (db.uiConfig.ring_tone_id == 0)
        db.uiConfig.ring_tone_id = 1;

    // update home panel bell text
    setBellText(db.uiConfig.alert_enabled, !db.silent);
    bool off = !db.uiConfig.alert_enabled && db.silent;
    Themes::recolorButton(objects.home_bell_button, !off);
    Themes::recolorText(objects.home_bell_label, !off);
    objects.home_bell_button->user_data = (void *)off;
}

void TFTView_320x240::updateTime(uint32_t timeVal)
{
    time_t localtime;
    time(&localtime);

    if (VALID_TIME(localtime)) {
        if (actTime != localtime) {
            ILOG_DEBUG("update (local)time: %d -> %d", actTime, localtime);
            actTime = localtime;
        }
    } else {
        if (timeVal > actTime) {
            ILOG_DEBUG("update (act)time: %d -> %d", actTime, timeVal);
            actTime = timeVal;
        }
    }
}

/**
 * @brief Create a new container for a node or group channel if it does not exist
 *
 * @param from
 * @param to: UINT32_MAX for broadcast, ownNode (= us) otherwise
 * @param channel
 */
lv_obj_t *TFTView_320x240::newMessageContainer(uint32_t from, uint32_t to, uint8_t ch)
{
    if (to == UINT32_MAX || from == 0) {
        if (channelGroup[ch] != nullptr)
            return channelGroup[ch];
    } else {
        auto it = messages.find(from);
        if (it != messages.end() && it->second)
            return it->second;
    }

    // create container for new messages
    lv_obj_t *container = lv_obj_create(objects.messages_panel);
    lv_obj_remove_style_all(container);
    lv_obj_set_width(container, lv_pct(100));
    lv_obj_set_height(container, lv_pct(88));
    lv_obj_set_x(container, 0);
    lv_obj_set_y(container, 0);
    lv_obj_set_align(container, LV_ALIGN_TOP_MID);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(container, lv_obj_flag_t(LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE |
                                               LV_OBJ_FLAG_SNAPPABLE | LV_OBJ_FLAG_SCROLL_ELASTIC)); /// Flags
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(container, LV_DIR_VER);
    lv_obj_set_style_pad_left(container, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(container, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(container, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // store new message container
    if (to == UINT32_MAX || from == 0) {
        channelGroup[ch] = container;
    } else {
        messages[from] = container;
    }

    // optionally add chat to chatPanel to access the container
    addChat(from, to, ch);

    return container;
}

/**
 * @brief insert a mew message that arrived into a <channel group> or <from node> container
 *
 * @param from source node
 * @param to destination node
 * @param ch channel
 * @param size length of msg
 * @param msg text message
 * @param time in/out: message time (maybe overwritten when 0)
 * @param restore if restoring then skip banners and highlight
 */
void TFTView_320x240::newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgTime, bool restore)
{
    ILOG_DEBUG("newMessage: from:0x%08x, to:0x%08x, ch:%d, time:%d", from, to, ch, msgTime);
    int pos = 0;
    char buf[284]; // 237 + 4 + 40 + 2 + 1
    lv_obj_t *container = nullptr;
    if (to == UINT32_MAX) { // message for group, prepend short name to msg
        if (nodes.find(from) == nodes.end()) {
            pos += sprintf(buf, "%04x ", from & 0xffff);
        } else {
            // original short name is held in userData, extract it and add msg
            char *userData = (char *)&(nodes[from]->LV_OBJ_IDX(node_lbs_idx)->user_data);
            while (pos < 4 && userData[pos] != 0) {
                buf[pos] = userData[pos];
                pos++;
            }
        }
        buf[pos++] = ' ';
        container = channelGroup[ch];
    } else { // message for us
        container = messages[from];
    }

    // if it's the first message we need a container
    if (!container) {
        container = newMessageContainer(from, to, ch);
    }

    pos += timestamp(&buf[pos], msgTime, !restore);
    sprintf(&buf[pos], "%s", msg);

    // place message into container
    newMessage(from, container, ch, buf);

    if (!restore) {
        // "Viewing" a chat requires the Meshtastic screen to actually be on display AND lit.
        // The chat panel stays "active" behind the launcher/apps/lock screen after leaving the
        // Mesh app, so without these checks messages arriving for that chat were treated as
        // already read (unread counter + popup went dead after the first visit).
        bool onMeshScreen = lv_screen_active() == objects.main_screen && !tdeck_input_gated && !tdeck_hold_dark;
        bool viewingThisChat =
            onMeshScreen && activePanel == objects.messages_panel && container == activeMsgContainer;
        if (!viewingThisChat) {
            unreadMessages++;
            updateUnreadMessages();
            if ((!onMeshScreen || activePanel != objects.messages_panel) && db.uiConfig.alert_enabled &&
                !db.channel[ch].settings.module_settings.is_muted) {
                showMessagePopup(from, to, ch, lv_label_get_text(nodes[from]->LV_OBJ_IDX(node_lbl_idx)));
            }
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        }
        if (container != activeMsgContainer)
            highlightChat(from, to, ch);
    } else {
        if (container != activeMsgContainer)
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief Display message bubble in related message container
 *
 * @param nodeNum
 * @param container
 * @param ch
 * @param msg
 */
void TFTView_320x240::newMessage(uint32_t nodeNum, lv_obj_t *container, uint8_t ch, const char *msg)
{
    lv_obj_t *hiddenPanel = lv_obj_create(container);
    lv_obj_set_width(hiddenPanel, lv_pct(100));
    lv_obj_set_height(hiddenPanel, LV_SIZE_CONTENT); /// 50
    lv_obj_set_align(hiddenPanel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(hiddenPanel, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_set_style_radius(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    add_style_panel_style(hiddenPanel);
    lv_obj_set_style_pad_left(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(hiddenPanel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *msgLabel = lv_label_create(hiddenPanel);
    // calculate expected size of text bubble, to make it look nicer
    lv_coord_t width = lv_txt_get_width(msg, strlen(msg), &ui_font_montserrat_14, 0);
    lv_obj_set_width(msgLabel, std::max<int32_t>(std::min<int32_t>((int32_t)(width), 160) + 10, 40));
    lv_obj_set_height(msgLabel, LV_SIZE_CONTENT);
    lv_obj_set_align(msgLabel, LV_ALIGN_LEFT_MID);
    lv_label_set_text(msgLabel, msg);
    add_style_new_message_style(msgLabel);
    lv_obj_add_flag(msgLabel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(msgLabel, ui_event_chatNodeButton, LV_EVENT_CLICKED, (void *)nodeNum);

    if (state == MeshtasticView::eRunning) {
        lv_obj_scroll_to_view(hiddenPanel, LV_ANIM_ON);
        lv_obj_move_foreground(objects.message_input_area);
    }
}

/**
 * restore messages from persistent log
 */
void TFTView_320x240::restoreMessage(const LogMessage &msg)
{
    //((uint8_t *)msg.bytes)[msg._size] = 0;
    // ILOG_DEBUG("restoring msg from:0x%08x, to:0x%08x, ch:%d, time:%d, status:%d, trash:%d, size:%d, '%s'", msg.from, msg.to,
    //           msg.ch, msg.time, (int)msg.status, msg.trashFlag, msg._size, msg.bytes);

    if (msg.from == ownNode) {
        lv_obj_t *container = nullptr;
        if (msg.to == UINT32_MAX) {
            if (msg.trashFlag && chats.find(msg.ch) != chats.end()) {
                ILOG_DEBUG("trashFlag set for channel %d", msg.ch);
                eraseChat(msg.ch);
                return;
            } else {
                container = newMessageContainer(msg.from, msg.to, msg.ch);
            }
        } else {
            if (nodes.find(msg.to) != nodes.end()) {
                if (msg.trashFlag && chats.find(msg.to) != chats.end()) {
                    ILOG_DEBUG("trashFlag set for node %08x", msg.to);
                    eraseChat(msg.to);
                    return;
                } else {
                    container = newMessageContainer(msg.to, msg.from, msg.ch);
                }
            } else {
                ILOG_DEBUG("to node 0x%08x not in db", msg.to);
                MeshtasticView::addOrUpdateNode(msg.to, msg.ch, 0, eRole::unknown, false, false);
            }
        }
        if (container) {
            if (container != activeMsgContainer)
                lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
            addMessage(container, msg.time, 0, (char *)msg.bytes, msg.status);
        }
    } else if (nodes.find(msg.from) != nodes.end()) {
        if (msg.trashFlag && chats.find(msg.from) != chats.end()) {
            ILOG_DEBUG("trashFlag set for node %08x", msg.from);
            eraseChat(msg.from);
            return;
        } else {
            uint32_t time = msg.time ? msg.time : UINT32_MAX; // don't overwrite 0 with actual time
            newMessage(msg.from, msg.to, msg.ch, (const char *)msg.bytes, time);
        }
    } else {
        int pos = 0;
        char buf[284]; // 237 + 4 + 40 + 2 + 1
        if (msg.to != UINT32_MAX) {
            // from node not in db
            ILOG_DEBUG("from node 0x%08x not in db", msg.from);
            MeshtasticView::addOrUpdateNode(msg.from, msg.ch, 0, eRole::unknown, false, false);
        } else {
            ILOG_DEBUG("from node 0x%08x not in db and no need to insert", msg.from);
            pos += sprintf(buf, "%04x ", msg.from & 0xffff);
        }
        uint32_t len = timestamp(buf + pos, msg.time, false);
        memcpy(buf + pos + len, msg.bytes, msg.length());
        buf[pos + len + msg.length()] = 0;

        lv_obj_t *container = newMessageContainer(msg.from, msg.to, msg.ch);
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        newMessage(msg.from, container, msg.ch, buf);
    }
}

/**
 * @brief Add a new chat to the chat panel to access the message container
 *
 * @param from
 * @param to
 * @param ch
 */
void TFTView_320x240::addChat(uint32_t from, uint32_t to, uint8_t ch)
{
    uint32_t index = ((to == UINT32_MAX || from == 0) ? ch : from);
    auto it = chats.find(index);
    if (it != chats.end())
        return;

    lv_obj_t *chatDelBtn = nullptr;
    lv_obj_t *parent_obj = objects.chats_panel;

    // ChatsButton
    lv_obj_t *chatBtn = lv_btn_create(parent_obj);
    lv_obj_set_pos(chatBtn, 0, 0);
    lv_obj_set_size(chatBtn, LV_PCT(100), buttonSize);
    lv_obj_add_flag(chatBtn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_clear_flag(chatBtn, LV_OBJ_FLAG_SCROLLABLE);
    add_style_home_button_style(chatBtn);
    lv_obj_set_style_align(chatBtn, LV_ALIGN_TOP_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(chatBtn, colorMidGray, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(chatBtn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_x(chatBtn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_ofs_y(chatBtn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(chatBtn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(chatBtn, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(chatBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(chatBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(chatBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(chatBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(chatBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_to_index(chatBtn, 0);

    char buf[64];
    if (to == UINT32_MAX || from == 0) {
        sprintf(buf, "%d: %s", (int)ch, lv_label_get_text(channel[ch]));
    } else {
        auto it = nodes.find(from);
        if (it != nodes.end()) {
            sprintf(buf, "%s: %s", lv_label_get_text(it->second->LV_OBJ_IDX(node_lbs_idx)),
                    lv_label_get_text(it->second->LV_OBJ_IDX(node_lbl_idx)));
        } else {
            sprintf(buf, "!%08x", from);
        }
    }

    {
        lv_obj_t *parent_obj = chatBtn;
        {
            // ChatsButtonLabel
            lv_obj_t *obj = lv_label_create(parent_obj);
            objects.chats_button_label = obj;
            lv_obj_set_pos(obj, 0, 0);
            lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
            lv_label_set_text(obj, buf);
            lv_obj_set_style_align(obj, LV_ALIGN_LEFT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        {
            // ChatDelButton
            lv_obj_t *obj = lv_btn_create(parent_obj);
            chatDelBtn = obj;
            lv_obj_set_pos(obj, -3, -1);
            lv_obj_set_size(obj, 40, 23);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_align(obj, LV_ALIGN_RIGHT_MID, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(obj, colorDarkRed, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            {
                lv_obj_t *parent_obj = obj;
                {
                    // DelLabel
                    lv_obj_t *chatDelBtn = lv_label_create(parent_obj);
                    lv_obj_set_pos(chatDelBtn, 0, 0);
                    lv_obj_set_size(chatDelBtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_label_set_text(chatDelBtn, _("DEL"));
                    lv_obj_set_style_align(chatDelBtn, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
    }

    chats[index] = chatBtn;
    updateActiveChats();
    if (index > c_max_channels) {
        if (nodes.find(index) != nodes.end())
            applyNodesFilter(index);
    }

    lv_obj_add_event_cb(chatBtn, ui_event_ChatButton, LV_EVENT_ALL, (void *)index);
    lv_obj_add_event_cb(chatDelBtn, ui_event_ChatDelButton, LV_EVENT_CLICKED, (void *)index);
}

void TFTView_320x240::highlightChat(uint32_t from, uint32_t to, uint8_t ch)
{
    uint32_t index = ((to == UINT32_MAX || from == 0) ? ch : from);
    auto it = chats.find(index);
    if (it != chats.end()) {
        // mark chat in color
        lv_obj_set_style_border_color(it->second, colorOrange, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void TFTView_320x240::updateActiveChats(void)
{
    char buf[48];
    sprintf(buf, _p("%d active chat(s)", chats.size()), chats.size());
    lv_label_set_text(objects.top_chats_label, buf);
}

/**
 * @brief Display banner showing to be patient while restoring messages
 */
void TFTView_320x240::notifyRestoreMessages(int32_t percentage)
{
    lv_bar_set_value(objects.message_restore_bar, percentage, LV_ANIM_OFF);
}

void TFTView_320x240::notifyMessagesRestored(void)
{
    MeshtasticView::notifyMessagesRestored();
    lv_obj_add_flag(objects.msg_restore_panel, LV_OBJ_FLAG_HIDDEN);
    updateActiveChats();
    updateNodesFiltered(true);
}

/**
 * @brief display new message popup panel
 *
 * @param from sender (NULL for removing popup)
 * @param to individual or group message
 * @param ch received channel
 */
void TFTView_320x240::showMessagePopup(uint32_t from, uint32_t to, uint8_t ch, const char *name)
{
    if (name) {
        static char buf[64];
        sprintf(buf, _("New message from \n%s"), name);
        buf[38] = '\0'; // cut too long userName
        lv_label_set_text(objects.msg_popup_label, buf);
        if (to == UINT32_MAX)
            objects.msg_popup_button->user_data = (void *)(uint32_t)ch; // store the channel in the button's data
        else
            objects.msg_popup_button->user_data = (void *)from; // store the node in the button's data
        lv_obj_clear_flag(objects.msg_popup_panel, LV_OBJ_FLAG_HIDDEN);

        if (db.module_config.external_notification.alert_message)
            lv_disp_trig_activity(NULL);

        lv_group_focus_obj(objects.msg_popup_button);
    }
}

void TFTView_320x240::hideMessagePopup(void)
{
    lv_obj_add_flag(objects.msg_popup_panel, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Display messages of a group channel
 *
 * @param ch
 */
void TFTView_320x240::showMessages(uint8_t ch)
{
    if (!messagesRestored) {
        // display message restoration progress banner
        lv_obj_clear_flag(objects.msg_popup_panel, LV_OBJ_FLAG_HIDDEN);
        lv_group_focus_obj(objects.msg_popup_button);
        return;
    }

    lv_obj_add_flag(activeMsgContainer, LV_OBJ_FLAG_HIDDEN);
    activeMsgContainer = channelGroup[ch];
    if (!activeMsgContainer) {
        activeMsgContainer = newMessageContainer(0, UINT32_MAX, ch);
    }

    activeMsgContainer->user_data = (void *)(uint32_t)ch;
    lv_obj_clear_flag(activeMsgContainer, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(objects.top_group_chat_label, lv_label_get_text(channel[ch]));
    ui_set_active(objects.messages_button, objects.messages_panel, objects.top_group_chat_panel);
}

/**
 * @brief Display messages from a node
 *
 * @param nodeNum
 */
void TFTView_320x240::showMessages(uint32_t nodeNum)
{
    lv_obj_add_flag(activeMsgContainer, LV_OBJ_FLAG_HIDDEN);
    activeMsgContainer = messages[nodeNum];
    if (!activeMsgContainer) {
        activeMsgContainer = newMessageContainer(nodeNum, 0, 0);
    }
    activeMsgContainer->user_data = (void *)nodeNum;
    lv_obj_clear_flag(activeMsgContainer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *p = nodes[nodeNum];
    if (p) {
        lv_label_set_text(objects.top_messages_node_label, lv_label_get_text(p->LV_OBJ_IDX(node_lbl_idx)));
        ui_set_active(objects.messages_button, objects.messages_panel, objects.top_messages_panel);
        switch ((unsigned long)p->LV_OBJ_IDX(node_bat_idx)->user_data) {
        case 0:
            lv_obj_set_style_bg_image_src(objects.top_messages_node_image, &img_lock_channel_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case 1:
            lv_obj_set_style_bg_image_src(objects.top_messages_node_image, &img_lock_secure_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        default:
            lv_obj_set_style_bg_image_src(objects.top_messages_node_image, &img_lock_slash_image,
                                          LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        }
        unreadMessages = 0; // TODO: not all messages may be actually read
        updateUnreadMessages();
    } else {
        // TODO: log error
    }
}

/**
 * @brief Place keyboard at a suitable space above or below the text input area
 *
 * @param textArea
 */
void TFTView_320x240::showKeyboard(lv_obj_t *textArea)
{
    lv_area_t text_coords, kb_coords;
    lv_obj_get_coords(textArea, &text_coords);
    lv_obj_get_coords(objects.keyboard, &kb_coords);
    uint32_t kb_h = kb_coords.y2 - kb_coords.y1;
    uint32_t v = lv_display_get_vertical_resolution(displaydriver->getDisplay());

    if (textArea == objects.message_input_area) {
        // if keyboard is to be shown in message input area then scroll the panel using animation
        static auto panelAnimCB = [](void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); };
        static auto kbdAnimCB = [](void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); };

        static lv_anim_t a1;
        lv_area_t panel_coords;
        lv_obj_get_coords(objects.messages_panel, &panel_coords);

        lv_anim_init(&a1);
        lv_anim_set_var(&a1, objects.messages_panel);
        lv_anim_set_exec_cb(&a1, panelAnimCB);
        lv_anim_set_values(&a1, panel_coords.y1, panel_coords.y1 - kb_h);
        lv_anim_set_duration(&a1, 300);
        lv_anim_set_path_cb(&a1, lv_anim_path_linear);
        lv_anim_start(&a1);

        static lv_anim_t a2;
        lv_anim_init(&a2);
        lv_anim_set_var(&a2, objects.keyboard);
        lv_anim_set_exec_cb(&a2, kbdAnimCB);
        lv_anim_set_values(&a2, v, v - kb_h);
        lv_anim_set_duration(&a2, 300);
        lv_anim_set_path_cb(&a2, lv_anim_path_linear);
        lv_anim_start(&a2);
    } else {
        if (text_coords.y1 > kb_h + 30) {
            // if enough place above put under top panel
            lv_obj_set_pos(objects.keyboard, 0, 28);
        } else if ((text_coords.y1 + 10) > v / 2) {
            // if text area is at lower half then place above text area
            lv_obj_set_pos(objects.keyboard, 0, text_coords.y1 - kb_h - 2);
        } else {
            // place below text area
            lv_obj_set_pos(objects.keyboard, 0, text_coords.y2 + 3);
        }
    }
    lv_keyboard_set_textarea(objects.keyboard, textArea);
}

void TFTView_320x240::hideKeyboard(lv_obj_t *panel)
{
    lv_area_t kb_coords;
    lv_obj_get_coords(objects.keyboard, &kb_coords);
    uint32_t kb_h = kb_coords.y2 - kb_coords.y1;

    if (panel == objects.messages_panel) {
        static auto panelAnimCB = [](void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); };
        static auto kbdAnimCB = [](void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); };
        static auto deleted_cb = [](_lv_anim_t *) { lv_obj_add_flag(objects.keyboard, LV_OBJ_FLAG_HIDDEN); };

        static lv_anim_t a1;
        lv_area_t panel_coords;
        lv_obj_get_coords(panel, &panel_coords);

        lv_anim_init(&a1);
        lv_anim_set_var(&a1, panel);
        lv_anim_set_exec_cb(&a1, panelAnimCB);
        lv_anim_set_values(&a1, panel_coords.y1, panel_coords.y1 + kb_h);
        lv_anim_set_duration(&a1, 300);
        lv_anim_set_path_cb(&a1, lv_anim_path_linear);
        lv_anim_start(&a1);

        static lv_anim_t a2;
        lv_anim_init(&a2);
        lv_anim_set_var(&a2, objects.keyboard);
        lv_anim_set_exec_cb(&a2, kbdAnimCB);
        lv_anim_set_values(&a2, kb_coords.y1, kb_coords.y1 + kb_h);
        lv_anim_set_duration(&a2, 300);
        lv_anim_set_path_cb(&a2, lv_anim_path_linear);
        lv_anim_set_deleted_cb(&a2, deleted_cb);
        lv_anim_start(&a2);
    }
}

lv_obj_t *TFTView_320x240::showQrCode(lv_obj_t *parent, const char *data)
{
    lv_color_t bg_color = colorMesh;
    lv_color_t fg_color = lv_palette_darken(LV_PALETTE_BLUE, 4);
    qr = lv_qrcode_create(parent);
    int32_t size = std::min<int32_t>(lv_obj_get_width(parent), lv_obj_get_height(parent)) - 8;
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, fg_color);
    lv_qrcode_set_light_color(qr, bg_color);
    lv_qrcode_update(qr, data, strlen(data));
    lv_obj_center(qr);
    lv_obj_set_style_border_color(qr, fg_color, 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    return qr;
}

/**
 * Enable underlying panel, buttons and scrollbar after it was disabled
 */
void TFTView_320x240::enablePanel(lv_obj_t *panel)
{
    lv_obj_clear_state(panel, LV_STATE_DISABLED);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    auto enableButtons = [](lv_obj_t *obj, void *) -> lv_obj_tree_walk_res_t {
        if (obj->class_p == &lv_button_class) {
            lv_obj_clear_state(obj, LV_STATE_DISABLED);
        }
        return LV_OBJ_TREE_WALK_NEXT;
    };

    lv_obj_tree_walk(panel, enableButtons, NULL);
}

/**
 * Disable underlying panel with it's children buttons and scrollbar
 */
void TFTView_320x240::disablePanel(lv_obj_t *panel)
{
    lv_obj_add_state(panel, LV_STATE_DISABLED);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    auto disableButtons = [](lv_obj_t *obj, void *) -> lv_obj_tree_walk_res_t {
        if (obj->class_p == &lv_button_class) {
            lv_obj_add_state(obj, LV_STATE_DISABLED);
        }
        return LV_OBJ_TREE_WALK_NEXT;
    };

    lv_obj_tree_walk(panel, disableButtons, NULL);
}

/**
 * Set focus to first button of a panel
 */
void TFTView_320x240::setGroupFocus(lv_obj_t *panel)
{
    if (panel == objects.home_panel) {
        lv_group_focus_obj(objects.home_mail_button);
    } else if (panel == objects.nodes_panel) {
        lv_group_focus_obj(objects.node_button);
    } else if (panel == objects.groups_panel) {
        lv_group_focus_obj(objects.channel_button0);
    } else if (panel == objects.messages_panel) {
        lv_group_focus_obj(objects.message_input_area);
    } else if (panel == objects.chats_panel) {
        if (chats.size() > 0) {
            lv_group_focus_obj(panel->spec_attr->children[1]); // TODO: does not work
        }
    } else if (panel == objects.map_panel) {

    } else if (panel == objects.settings_screen_lock_panel) {
        lv_group_focus_obj(objects.screen_lock_button_matrix);
    } else if (panel == objects.controller_panel) {
        lv_group_focus_obj(objects.basic_settings_user_button);
    } else {
        for (int i = 0; i < lv_obj_get_child_count(panel); i++) {
            if (panel->spec_attr->children[i]->class_p == &lv_button_class) {
                lv_group_focus_obj(panel->spec_attr->children[i]);
                break;
            }
        }
    }
}

/**
 * input group used by keyboard and/or pointer for dynamic assignment
 */
void TFTView_320x240::setInputGroup(void)
{
    lv_group_t *group = lv_group_get_default();

    if (group && inputdriver->hasKeyboardDevice())
        lv_indev_set_group(inputdriver->getKeyboard(), group);

    if (group && inputdriver->hasPointerDevice())
        lv_indev_set_group(inputdriver->getPointer(), group);
}

void TFTView_320x240::setInputButtonLabel(void)
{
    // update input button label
    std::string current_kbd = inputdriver->getCurrentKeyboardDevice();
    std::string current_ptr = inputdriver->getCurrentPointerDevice();

    char label[40];
    lv_snprintf(label, sizeof(label), _("Input Control: %s/%s"), current_ptr.c_str(), current_kbd.c_str());
    lv_label_set_text(objects.basic_settings_input_label, label);
}
// -------- helpers --------

void TFTView_320x240::removeNode(uint32_t nodeNum)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end()) {
    }
}

void TFTView_320x240::setNodeImage(uint32_t nodeNum, eRole role, bool unmessagable, lv_obj_t *img)
{
    uint32_t bgColor, fgColor;
    std::tie(bgColor, fgColor) = nodeColor(nodeNum);
    if (unmessagable) {
        lv_image_set_src(img, &img_unmessagable_image);
        lv_obj_set_style_border_color(img, lv_color_hex(bgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(img, lv_color_hex(0x202020), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor(img, lv_color_hex(0xFF5555), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_img_recolor_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    } else {
        switch (role) {
        case client:
        case client_mute:
        case client_hidden:
        case tak: {
            lv_image_set_src(img, &img_node_client_image);
            break;
        }
        case router_client: {
            lv_image_set_src(img, &img_top_nodes_image);
            break;
        }
        case repeater:
        case router:
        case router_late: {
            lv_image_set_src(img, &img_node_router_image);
            break;
        }
        case tracker:
        case sensor:
        case lost_and_found:
        case tak_tracker: {
            lv_image_set_src(img, &img_node_sensor_image);
            break;
        }
        case unknown: {
            lv_image_set_src(img, &img_circle_question_image);
            break;
        }
        default:
            lv_image_set_src(img, &img_node_client_image);
            break;
        }
    }
    lv_obj_set_style_bg_color(img, lv_color_hex(bgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(img, lv_color_hex(bgColor), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_img_recolor_opa(img, fgColor ? 0 : 255, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void TFTView_320x240::updateNodesStatus(void)
{
    char buf[40];
    lv_snprintf(buf, sizeof(buf), _p("%d of %d nodes online", nodeCount), nodesOnline, nodeCount);
    lv_label_set_text(objects.home_nodes_label, buf);

    if (nodesFiltered)
        lv_snprintf(buf, sizeof(buf), _("Filter: %d of %d nodes"), nodeCount - nodesFiltered, nodeCount);
    lv_label_set_text(objects.top_nodes_online_label, buf);
}

/**
 * @brief Dynamically update all nodes filter and highlight
 *        Because the update can take quite some time (tens of ms) it is done in smaller
 *        chunks of 10 nodes per invocation, so it must be periodically called
 *        TODO: check for side effects if new nodes are inserted or removed during filter processing
 * @param reset indicates to start update from beginning of node list otherwise
 *        continue with iterator position or skip if done
 */
void TFTView_320x240::updateNodesFiltered(bool reset)
{
    static auto it = nodes.begin();
    if (reset || nodesChanged) {
        nodesFiltered = 0;
        nodesChanged = false;
        processingFilter = true;
        it = nodes.begin();
    }

    for (int i = 0; i < 10 && it != nodes.end(); i++) {
        applyNodesFilter(it->first, true);
        it++;
    }

    if (it == nodes.end()) {
        processingFilter = false;
    }
    updateNodesStatus();
}

/**
 * @brief Update last heard display/user_data/counter to current time
 *
 * @param nodeNum
 */
void TFTView_320x240::updateLastHeard(uint32_t nodeNum)
{
    auto it = nodes.find(nodeNum);
    if (it != nodes.end() && it->second) {
        time_t lastHeard = (time_t)it->second->LV_OBJ_IDX(node_lh_idx)->user_data;
        it->second->LV_OBJ_IDX(node_lh_idx)->user_data = (void *)curtime;
        lv_label_set_text(it->second->LV_OBJ_IDX(node_lh_idx), _("now"));
        if (it->first != ownNode) {
            if (lastHeard > 0 && curtime - lastHeard >= secs_until_offline) {
                nodesOnline++;
                applyNodesFilter(nodeNum);
                updateNodesStatus();
            }
            // move to top position
            lv_obj_move_to_index(it->second, 1);

            // re-arrange the group linked list i.e. move the node after the top position
            lv_ll_t *lv_group_ll = &lv_group_get_default()->obj_ll;
            void *act = it->second->LV_OBJ_IDX(node_btn_idx)->user_data;
            if (lv_group_ll && act)
                _lv_ll_move_before(lv_group_ll, act, _lv_ll_get_next(lv_group_ll, topNodeLL));
        }
    }
}

/**
 * @brief update last heard display for all nodes; also update nodes online
 *
 */
void TFTView_320x240::updateAllLastHeard(void)
{
    uint16_t online = 0;
    time_t lastHeard;
    for (auto it : nodes) {
        char buf[32];
        if (it.first == ownNode) { // own node is always now, so do update
            lastHeard = curtime;
            it.second->LV_OBJ_IDX(node_lh_idx)->user_data = (void *)lastHeard;
        } else {
            lastHeard = (time_t)it.second->LV_OBJ_IDX(node_lh_idx)->user_data;
        }
        if (lastHeard) {
            bool isOnline = lastHeardToString(lastHeard, buf);
            lv_label_set_text(it.second->LV_OBJ_IDX(node_lh_idx), buf);
            if (isOnline)
                online++;
        }
    }
    nodesOnline = online;
    updateNodesFiltered(true);
    updateNodesStatus();
}

void TFTView_320x240::updateUnreadMessages(void)
{
    char buf[64];
    if (unreadMessages > 0) {
        sprintf(buf, unreadMessages == 1 ? _("%d new message") : _("%d new messages"), unreadMessages);
        lv_obj_set_style_bg_img_src(objects.home_mail_button, &img_home_mail_unread_button_image,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        strcpy(buf, _("no new messages"));
        lv_obj_set_style_bg_img_src(objects.home_mail_button, &img_home_mail_button_image, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_label_set_text(objects.home_mail_label, buf);

    // Mirror the count into the launcher top bar (next to "mesh") and the lock pad's top-left;
    // blank when there are none, so both clear the moment messages are opened (unreadMessages is
    // reset to 0 on that path).
    char top[24];
    if (unreadMessages > 0)
        snprintf(top, sizeof(top), unreadMessages == 1 ? "%lu msg" : "%lu msgs", (unsigned long)unreadMessages);
    else
        top[0] = '\0';
    if (launcher_unread_label)
        lv_label_set_text(launcher_unread_label, top);
    if (lockpad_unread_label)
        lv_label_set_text(lockpad_unread_label, top);
}

/**
 * @brief Called once a second to update time label
 *
 */
void TFTView_320x240::updateTime(void)
{
    char buf[80];
    time_t curr_time;
#ifdef ARCH_PORTDUINO
    time(&curr_time);
#else
    curr_time = actTime;
#endif
    tm *curr_tm = localtime(&curr_time);

    int len = 0;
    if (VALID_TIME(curr_time) && (unsigned long)objects.home_time_button->user_data == 0) {
        if (db.config.display.use_12h_clock) {
            len = strftime(buf, 40, "%I:%M:%S %p\n%a %d-%b-%g", curr_tm);
        } else {
            len = strftime(buf, 40, "%T %Z%z\n%a %d-%b-%g", curr_tm);
        }
    } else {
        uint32_t uptime = millis() / 1000;
        int hours = uptime / 3600;
        uptime -= hours * 3600;
        int minutes = uptime / 60;
        int seconds = uptime - minutes * 60;

        sprintf(&buf[len], _("uptime: %02d:%02d:%02d"), hours, minutes, seconds);
    }
    lv_label_set_text(objects.home_time_label, buf);
}

bool TFTView_320x240::updateSDCard(void)
{
    formatSD = false;
    if (sdCard) {
        delete sdCard;
        sdCard = nullptr;
    }
#ifdef HAS_SDCARD
    char buf[64];
#ifdef HAS_SD_MMC
    sdCard = new SDCard;
#else
    sdCard = new SdFsCard;
#endif
    ISdCard::ErrorType err = ISdCard::ErrorType::eNoError;
    if (sdCard->init() && sdCard->cardType() != ISdCard::eNone) {
        ILOG_DEBUG("SdCard init successful, card type: %d", sdCard->cardType());
        ISdCard::CardType cardType = sdCard->cardType();
        ISdCard::FatType fatType = sdCard->fatType();
        uint32_t usedSpace = sdCard->usedBytes() / (1024 * 1024);
        uint32_t totalSpace = sdCard->cardSize() / (1024 * 1024);
        uint32_t totalSpaceGB = (sdCard->cardSize() + 500000000ULL) / (1000ULL * 1000ULL * 1000ULL);

        sprintf(buf, _("%s: %d GB (%s)\nUsed: %0.2f GB (%d%%)"),
                cardType == ISdCard::eMMC    ? "MMC"
                : cardType == ISdCard::eSD   ? "SDSC"
                : cardType == ISdCard::eSDHC ? "SDHC"
                : cardType == ISdCard::eSDXC ? "SDXC"
                                             : "UNKN",
                totalSpaceGB,
                fatType == ISdCard::eExFat   ? "exFAT"
                : fatType == ISdCard::eFat32 ? "FAT32"
                : fatType == ISdCard::eFat16 ? "FAT16"
                                             : "???",
                float(sdCard->usedBytes()) / 1024.0f / 1024.0f / 1024.0f,
                totalSpace ? ((usedSpace * 100) + totalSpace / 2) / totalSpace : 0);
        Themes::recolorButton(objects.home_sd_card_button, true);
        Themes::recolorText(objects.home_sd_card_label, true);
        cardDetected = true;
    } else {
        ILOG_DEBUG("SdFsCard init failed");
        err = sdCard->errorType();
        delete sdCard;
        sdCard = nullptr;
    }

    if (!cardDetected || err != ISdCard::ErrorType::eNoError) {
        switch (err) {
        case ISdCard::ErrorType::eSlotEmpty:
            ILOG_ERROR("SD card slot empty");
            lv_snprintf(buf, sizeof(buf), _("SD slot empty"));
            break;
        case ISdCard::ErrorType::eFormatError:
            ILOG_ERROR("SD invalid format");
            lv_snprintf(buf, sizeof(buf), _("SD invalid format"));
            formatSD = true;
            break;
        case ISdCard::ErrorType::eNoMbrError:
            ILOG_ERROR("SD mbr not found");
            lv_snprintf(buf, sizeof(buf), _("SD mbr not found"));
            formatSD = true;
            break;
        case ISdCard::ErrorType::eCardError:
            ILOG_ERROR("SD card error");
            lv_snprintf(buf, sizeof(buf), _("SD card error"));
            break;
        default:
            ILOG_ERROR("SD unknown error");
            lv_snprintf(buf, sizeof(buf), _("SD unknown error"));
            break;
        }
        Themes::recolorButton(objects.home_sd_card_button, false);
        Themes::recolorText(objects.home_sd_card_label, false);
        // allow backup/restore only if there is an SD card detected
        lv_obj_add_state(objects.basic_settings_backup_restore_button, LV_STATE_DISABLED);
    } else {
        // enable backup/restore
        lv_obj_clear_state(objects.basic_settings_backup_restore_button, LV_STATE_DISABLED);
    }
    lv_label_set_text(objects.home_sd_card_label, buf);
#else
    lv_obj_add_flag(objects.home_sd_card_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(objects.home_sd_card_label, LV_OBJ_FLAG_HIDDEN);
#if defined(ARCH_PORTDUINO)
    cardDetected = true; // use PortduinoFS instead
    sdCard = new SDCard;
#endif
#endif
    if (!sdCard)
        sdCard = new NoSdCard;
    return cardDetected;
}

void TFTView_320x240::formatSDCard(void)
{
    if (sdCard) {
        delete sdCard;
        sdCard = nullptr;
    }
#ifdef HAS_SDCARD
#ifdef HAS_SD_MMC
    sdCard = new SDCard;
#else
    sdCard = new SdFsCard;
#endif
    ILOG_DEBUG("formatting SD card");
    if (sdCard->format()) {
        updateSDCard();
    } else {
        lv_label_set_text(objects.home_sd_card_label, "SD format failed");
    }
#endif
    if (!sdCard)
        sdCard = new NoSdCard;
}

void TFTView_320x240::updateFreeMem(void)
{
    // only update if HomePanel is active (since this is some critical code that did crash sporadically)
    if (activePanel == objects.home_panel && (unsigned long)objects.home_memory_button->user_data) {
        char buf[64];
        uint32_t freeHeap = 0;
        uint32_t freeHeap_pct = 0;

        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);

#ifdef ARDUINO_ARCH_ESP32
        freeHeap = ESP.getFreeHeap();
        freeHeap_pct = 100 * freeHeap / ESP.getHeapSize();
        sprintf(buf, _("Heap: %d (%d%%)\nLVGL: %d (%d%%)"), freeHeap, freeHeap_pct, mon.free_size, 100 - mon.used_pct);
#elif defined(ARCH_PORTDUINO)
        static uint32_t totalMem = LinuxHelper::getTotalMem();
        if (totalMem != 0) {
            freeHeap = LinuxHelper::getAvailableMem();
            freeHeap_pct = 100 * freeHeap / totalMem;
        }
        sprintf(buf, _("Heap: %d (%d%%)\nLVGL: %d (%d%%)"), freeHeap, freeHeap_pct, mon.free_size / 1024, 100 - mon.used_pct);
#else
        buf[0] = '\0';
#endif
        lv_label_set_text(objects.home_memory_label, buf);
    }
}

void TFTView_320x240::task_handler(void)
{
    MeshtasticView::task_handler();

    if (screensInitialised) {
        // exactly ONE map may pump its redraws at a time (MapPanel::redraw() keeps
        // function-local statics), so gate each on its screen actually being visible
        if (map && activePanel == objects.map_panel && lv_screen_active() == objects.main_screen)
            map->task_handler();
        if (userMap && maps_screen && lv_screen_active() == maps_screen)
            userMap->task_handler();

        if (curtime - lastrun1 >= 1) { // call every 1s
            if (map) {
                updateLocationMap(THIS->map->getObjectsOnMap());
            }

            lastrun1 = curtime;
            actTime++;
            updateTime();

            if (curtime - lastrun5 >= 5) { // call every 5s
                lastrun5 = curtime;
                if (scans > 0 && activePanel == objects.signal_scanner_panel) {
                    scanSignal(scans);
                    scans--;
                }
                if (startTime) {
                    if (curtime - startTime > 30) {
                        lv_label_set_text(objects.trace_route_start_label, _("Start"));
                        lv_obj_set_style_outline_color(objects.trace_route_start_button, colorMesh,
                                                       LV_PART_MAIN | LV_STATE_DEFAULT);
                        removeSpinner();
                    } else {
                        char buf[16];
                        sprintf(buf, "%ds", ((35 - (curtime - startTime)) / 5) * 5);
                        lv_label_set_text(objects.trace_route_start_label, buf);
                    }
                }
            }
            if (curtime - lastrun10 >= 10) { // call every 10s
                lastrun10 = curtime;
                updateFreeMem();

                if ((db.config.network.wifi_enabled || db.module_config.mqtt.enabled) && !displaydriver->isPowersaving()) {
                    controller->requestDeviceConnectionStatus();
                }
            }
            if (curtime - lastrun60 >= 60) { // call every 60s
                lastrun60 = curtime;
                updateAllLastHeard();

                if (detectorRunning) {
                    controller->sendPing();
                }

                // if we didn't hear any node for 1h assume we have no signal
                if (curtime - lastHeard > secs_until_offline) {
                    lv_obj_set_style_bg_image_src(objects.home_signal_button, &img_home_no_signal_image,
                                                  LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text(objects.home_signal_label, _("no signal"));
                    lv_label_set_text(objects.home_signal_pct_label, "");
                }
            }
        }
        if (processingFilter || nodesChanged) {
            updateNodesFiltered(nodesChanged);
        }
    }
}

// === lvgl C style callbacks ===

extern "C" {

void action_on_boot_screen_displayed(lv_event_t *e)
{
    ILOG_DEBUG("action_on_boot_screen_displayed()");
}
}

#endif
