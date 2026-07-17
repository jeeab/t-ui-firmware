#pragma once

#include "graphics/common/MeshtasticView.h"
#include "meshtastic/clientonly.pb.h"
#include <set>

class MapPanel;
class ITileService;

/**
 * @brief GUI view for e.g. T-Deck
 * Handles creation of display driver and controller.
 * Note: due to static callbacks in lvgl this class is modelled as
 *       a singleton with static callback members
 */
class TFTView_320x240 : public MeshtasticView
{
  public:
    void init(IClientBase *client) override;
    bool setupUIConfig(const meshtastic_DeviceUIConfig &uiconfig) override;
    void task_handler(void) override;

    // Launcher tile actions must be public statics (kApps[] lives at file scope and
    // can't reach private members). They route to the private instance methods.
    static void openSettingsAction(void);
    static void openFilesAction(void);
    static void openFlashlightAction(void);
    static void openMapsAction(void); // Maps tile -> our own standalone Maps app screen

    // methods to update view
    void setMyInfo(uint32_t nodeNum) override;
    void setDeviceMetaData(int hw_model, const char *version, bool has_bluetooth, bool has_wifi, bool has_eth,
                           bool can_shutdown) override;
    void addOrUpdateNode(uint32_t nodeNum, uint8_t channel, uint32_t lastHeard, const meshtastic_User &cfg) override;
    void addNode(uint32_t nodeNum, uint8_t channel, const char *userShort, const char *userLong, uint32_t lastHeard, eRole role,
                 bool hasKey, bool unmessagable) override;
    void updateNode(uint32_t nodeNum, uint8_t channel, const meshtastic_User &cfg) override;
    void updatePosition(uint32_t nodeNum, int32_t lat, int32_t lon, int32_t alt, uint32_t sats, uint32_t precision) override;
    void updateMetrics(uint32_t nodeNum, uint32_t bat_level, float voltage, float chUtil, float airUtil) override;
    void updateEnvironmentMetrics(uint32_t nodeNum, const meshtastic_EnvironmentMetrics &metrics) override;
    void updateAirQualityMetrics(uint32_t nodeNum, const meshtastic_AirQualityMetrics &metrics) override;
    void updatePowerMetrics(uint32_t nodeNum, const meshtastic_PowerMetrics &metrics) override;
    void updateSignalStrength(uint32_t nodeNum, int32_t rssi, float snr) override;
    void updateHopsAway(uint32_t nodeNum, uint8_t hopsAway) override;
    void updateConnectionStatus(const meshtastic_DeviceConnectionStatus &status) override;

    // methods to update device config
    void updateChannelConfig(const meshtastic_Channel &ch) override;
    void updateDeviceConfig(const meshtastic_Config_DeviceConfig &cfg) override;
    void updatePositionConfig(const meshtastic_Config_PositionConfig &cfg) override;
    void updatePowerConfig(const meshtastic_Config_PowerConfig &cfg) override;
    void updateNetworkConfig(const meshtastic_Config_NetworkConfig &cfg) override;
    void updateDisplayConfig(const meshtastic_Config_DisplayConfig &cfg) override;
    void updateLoRaConfig(const meshtastic_Config_LoRaConfig &cfg) override;
    void updateBluetoothConfig(const meshtastic_Config_BluetoothConfig &cfg, uint32_t id = 0) override;
    void updateSecurityConfig(const meshtastic_Config_SecurityConfig &cfg) override;
    void updateSessionKeyConfig(const meshtastic_Config_SessionkeyConfig &cfg) override;

    // methods to update module config
    void updateMQTTModule(const meshtastic_ModuleConfig_MQTTConfig &cfg) override;
    void updateSerialModule(const meshtastic_ModuleConfig_SerialConfig &cfg) override {}
    void updateExtNotificationModule(const meshtastic_ModuleConfig_ExternalNotificationConfig &cfg) override;
    void updateStoreForwardModule(const meshtastic_ModuleConfig_StoreForwardConfig &cfg) override {}
    void updateRangeTestModule(const meshtastic_ModuleConfig_RangeTestConfig &cfg) override {}
    void updateTelemetryModule(const meshtastic_ModuleConfig_TelemetryConfig &cfg) override {}
    void updateCannedMessageModule(const meshtastic_ModuleConfig_CannedMessageConfig &) override {}
    void updateAudioModule(const meshtastic_ModuleConfig_AudioConfig &cfg) override {}
    void updateRemoteHardwareModule(const meshtastic_ModuleConfig_RemoteHardwareConfig &cfg) override {}
    void updateNeighborInfoModule(const meshtastic_ModuleConfig_NeighborInfoConfig &cfg) override {}
    void updateAmbientLightingModule(const meshtastic_ModuleConfig_AmbientLightingConfig &cfg) override {}
    void updateDetectionSensorModule(const meshtastic_ModuleConfig_DetectionSensorConfig &cfg) override {}
    void updatePaxCounterModule(const meshtastic_ModuleConfig_PaxcounterConfig &cfg) override {}
    void updateFileinfo(const meshtastic_FileInfo &fileinfo) override {}
    void updateRingtone(const char rtttl[231]) override;

    // update internal time
    void updateTime(uint32_t time) override;

    void packetReceived(const meshtastic_MeshPacket &p) override;
    void handleResponse(uint32_t from, uint32_t id, const meshtastic_Routing &routing, const meshtastic_MeshPacket &p) override;
    void handleResponse(uint32_t from, uint32_t id, const meshtastic_RouteDiscovery &route) override;
    void handlePositionResponse(uint32_t from, uint32_t request_id, int32_t rx_rssi, float rx_snr, bool isNeighbor) override;
    void notifyRestoreMessages(int32_t percentage) override;
    void notifyMessagesRestored(void) override;
    void notifyConnected(const char *info) override;
    void notifyDisconnected(const char *info) override;
    void notifyResync(bool show) override;
    void notifyReboot(bool show) override;
    void notifyShutdown(void) override;
    void blankScreen(bool enable) override;
    void screenSaving(bool enabled) override;
    bool isScreenLocked(void) override;
    void newMessage(uint32_t from, uint32_t to, uint8_t ch, const char *msg, uint32_t &msgtime, bool restore = true) override;
    void restoreMessage(const LogMessage &msg) override;
    void removeNode(uint32_t nodeNum) override;

    enum BasicSettings {
        eNone,
        eSetup,
        eUsername,
        eDeviceRole,
        eRegion,
        eModemPreset,
        eChannel,
        eWifi,
        eLanguage,
        eScreenTimeout,
        eScreenLock,
        eScreenBrightness,
        eTheme,
        eInputControl,
        eAlertBuzzer,
        eBackupRestore,
        eReset,
        eReboot,
        eDisplayMode,
        eModifyChannel
    };

  protected:
    struct NodeFilter {
        bool unknown;  // filter out unknown nodes
        bool mqtt;     // filter out via mqtt nodes
        bool offline;  // filter out offline nodes (>15min lastheard)
        bool position; // filter out nodes without position
        char *name;    // filter by name
        bool active;   // flag for active filter
    };

    struct NodeHighlight {
        bool chat;      // highlight nodes with active chats
        bool position;  // highlight nodes with position
        bool telemetry; // highlight nodes with telemetry
        bool iaq;       // highlight nodes with IAQ
        char *name;     // hightlight by name
        bool active;    // flag for active highlight;
    };

    typedef void (*UserWidgetFunc)(lv_obj_t *, void *, int);

    // initialize all ui screens
    virtual void init_screens(void);
    // update custom display string on boot screen
    virtual void updateBootMessage(const char *);
    // show initial setup panel to configure region and name
    virtual void requestSetup(void);
    // patch widgets on generated screens
    virtual void apply_hotfix(void);
    // update node counter display (online and filtered)
    virtual void updateNodesStatus(void);
    // display message popup
    virtual void showMessagePopup(uint32_t from, uint32_t to, uint8_t ch, const char *name);
    // hide new message popup
    virtual void hideMessagePopup(void);
    // display user widget (dynamically created)
    void showUserWidget(UserWidgetFunc createWidget);
    // display messages of a group channel
    virtual void addChat(uint32_t from, uint32_t to, uint8_t ch);
    // mark chat border to indicate a new message
    virtual void highlightChat(uint32_t from, uint32_t to, uint8_t ch);
    // display number of active chats
    virtual void updateActiveChats(void);
    // display new message popup
    virtual void showMessages(uint8_t channel);
    // display messages of a node
    virtual void showMessages(uint32_t nodeNum);
    // own chat message
    virtual void handleAddMessage(char *msg);
    // add own message to current chat
    virtual void addMessage(lv_obj_t *container, uint32_t msgTime, uint32_t requestId, char *msg, LogMessage::MsgStatus status);
    // add new message to container
    virtual void newMessage(uint32_t nodeNum, lv_obj_t *container, uint8_t channel, const char *msg);
    // create empty message container for node or group channel
    virtual lv_obj_t *newMessageContainer(uint32_t from, uint32_t to, uint8_t ch);
    // filter or highlight node
    virtual bool applyNodesFilter(uint32_t nodeNum, bool reset = false);
    // display message alert popup
    virtual void messageAlert(const char *alert, bool show);
    // mark sent message as received
    virtual void handleTextMessageResponse(uint32_t channelOrNode, uint32_t id, bool ack, bool err);
    // set node image based on role
    virtual void setNodeImage(uint32_t nodeNum, eRole role, bool unmessagable, lv_obj_t *img);
    // apply filter and count number of filtered nodes
    virtual void updateNodesFiltered(bool reset);
    // set last heard to now, update nodes online
    virtual void updateLastHeard(uint32_t nodeNum);
    // update last heard value on all node panels
    virtual void updateAllLastHeard(void);
    // update image and unread messages on home screen
    virtual void updateUnreadMessages(void);
    // update time display on home screen
    virtual void updateTime(void);
    // update SD card slot info
    virtual bool updateSDCard(void);
    // format SD card if invalid
    virtual void formatSDCard(void);
    // update time display on home screen
    virtual void updateFreeMem(void);
    // update distance to other node
    virtual void updateDistance(uint32_t nodeNum, int32_t lat, int32_t lon);
    // show map and load tiles
    virtual void loadMap(void);
    // add objects on map
    virtual void addOrUpdateMap(uint32_t nodeNum, int32_t lat, int32_t lon);
    // remove objects from map
    virtual void removeFromMap(uint32_t nodeNum);

    std::function<void(uint32_t id, uint16_t x, uint16_t y, uint8_t)> drawObjectCB;

    // ---- Maps app: user-dropped pins on our own map screen (long-press to drop) ----
    struct MapPin {
        uint32_t id;
        float lat, lon;
        uint32_t color;
        uint32_t whenEpoch; // unix time dropped (0 = unknown)
        char label[24];
        lv_obj_t *marker;   // the on-map dot (child of maps_map_container)
        lv_obj_t *labelObj; // the pin's name shown next to the dot (also a child)
    };
    std::vector<MapPin> mapPins;
    uint32_t nextPinId = 1;
    bool pinsLoaded = false;
    lv_obj_t *pins_overlay = nullptr;
    std::function<void(uint32_t, uint16_t, uint16_t, uint8_t)> drawPinCB;
    MapPin *findPin(uint32_t id);
    lv_obj_t *makePinMarker(uint32_t color);
    lv_obj_t *makePinLabel(const char *name, uint32_t color); // name tag drawn beside a pin
    void dropPinAt(float lat, float lon);
    void drawAllPins(void);
    void loadPins(void);
    bool savePins(void); // returns false if the SD write failed (surfaced on-screen)
    void openPinsList(void);
    void closePinsList(void);
    void deletePin(uint32_t id);
    void centerOnPin(uint32_t id);
    void renamePin(uint32_t id, const char *name);
    void openRenamePin(uint32_t id);
    void closeRenamePin(void);
    lv_obj_t *rename_overlay = nullptr;
    lv_obj_t *rename_ta = nullptr;
    uint32_t renaming_pin_id = 0;
    // Keeps the physical keyboard aimed at whichever text box is open (pin rename / new folder).
    // Only one of those overlays is ever open at a time, so a single shared guard timer is fine.
    lv_timer_t *text_entry_guard = nullptr;
    lv_obj_t *text_entry_ta = nullptr; // the textarea the guard keeps focused
    void beginTextEntry(lv_obj_t *ta); // focus a textarea for the physical keyboard + start the guard
    void endTextEntry(void);           // stop the guard (call before deleting the textarea)

    // Crash diagnostics
    void showDiagnostics(void); // full readable memory/restart popup (tap the top-bar readout)
    void logDiagBoot(void);     // once-per-boot: log a fault line to SD /diaglog.txt
    void diagLog(const char *line);

    // ---- app grid: built-in apps + user apps discovered on the SD card (/apps/<name>/main.lua),
    //      shown in a user-chosen order (persisted to /apps/order.txt) and re-arrangeable. ----
    struct LaunchDesc {         // POD (header-safe: no anon-namespace types)
        char name[24];          // display label (also the ordering key)
        uint32_t color;
        int builtinIdx;         // >=0 => kApps[builtinIdx]; -1 => a user SD app
        int userIdx;            // >=0 => userAppDirs[userIdx]; -1 => a built-in app
    };
    static const int kMaxUserApps = 12;
    char userAppDirs[kMaxUserApps][24]; // folder names under /apps that hold a main.lua
    int userAppCount = 0;
    LaunchDesc launchList[40];           // the assembled, ordered grid (stable storage for tiles)
    int launchCount = 0;
    bool suppressTileClick = false;      // a long-press opened Arrange -> don't also launch on release
    lv_obj_t *arrange_overlay = nullptr;
    lv_obj_t *arrange_list = nullptr;    // the scrollable row list inside the overlay
    void refreshArrangeRows(void);       // re-label rows in place after a move (keeps scroll pos)
    void scanUserApps(void);             // find user apps on the SD card (once at boot)
    void runUserApp(int userIdx);        // launch a discovered SD app by index
    void buildAppGrid(void);             // assemble launchList + build the pager/tiles
    void rebuildAppGrid(void);           // tear down + rebuild the grid live (after reorder)
    void applyAppOrder(void);            // reorder launchList by /apps/order.txt
    void saveAppOrder(void);             // persist the current order to /apps/order.txt
    void openArrange(void);              // "Arrange apps" overlay (long-press a tile)
    void closeArrange(void);
    void moveAppInArrange(int idx, int delta);

    NodeFilter filter;
    NodeHighlight highlight;

  private:
    // view creation only via ViewFactory
    friend class ViewFactory;
    static TFTView_320x240 *instance(void);
    static TFTView_320x240 *instance(const DisplayDriverConfig &cfg);
    TFTView_320x240();
    TFTView_320x240(const DisplayDriverConfig *cfg, DisplayDriver *driver);

    void enterProgrammingMode(void);
    void exitProgrammingMode(void); // disable BT + reboot; reachable by tap OR trackball double-click
    void updateTheme(void);
    void ui_events_init(void);

    // ---- custom iPhone-style app-grid launcher (boots first; tiles open MUI screens) ----
    void createLauncher(void);
    lv_obj_t *launcher_screen = nullptr;
    lv_obj_t *splash_title = nullptr;        // "T-UI" boot label; hidden on the programming screen
    // Flashlight app: full-white screen at max backlight; restores brightness on exit
    void openFlashlight(void);
    lv_obj_t *flashlight_screen = nullptr;
    lv_timer_t *flashlight_keepawake = nullptr; // pokes activity so it never idle-dims
    uint32_t flashlight_saved_brightness = 153;
    // ---- Maps app: our own standalone map screen (same SD tiles, own MapPanel instance) ----
    void openMaps(void);
    void mapsInitTileStyle(void);                 // detect tile style/prefix on SD (mirror of loadMap's logic)
    static ITileService *sharedTileService(void); // ONE tile service ever — its ctor registers an LVGL fs driver
    lv_obj_t *maps_screen = nullptr;
    lv_obj_t *maps_map_container = nullptr; // the tile canvas; pin markers are its children
    lv_obj_t *maps_gps_dot = nullptr;       // "you are here" dot
    MapPanel *userMap = nullptr;            // our own map instance (mesh map keeps its own, `map`)
    bool mapsStyleInited = false;
    bool meshMapStale = false; // Maps app touched shared zoom/tile state; mesh map re-syncs on next load
    void updateMapsSats(void);              // refresh the top-bar satellite readout (1s timer)
    void mapsShowNotice(const char *msg);   // transient on-map message (auto-hides)
    lv_obj_t *maps_sats_label = nullptr;    // top-bar "N sats" readout (green = locked)
    lv_timer_t *maps_sats_timer = nullptr;  // drives updateMapsSats while Maps is on screen
    lv_obj_t *maps_notice = nullptr;        // the transient message label
    uint32_t maps_notice_until = 0;         // lv_tick deadline to hide it (0 = hidden)
    // Maps: style picker (gear cog) + USGS region downloader
    void openMapsMenu(void);                    // gear -> style list + "Download this area"
    void closeMapsMenu(void);
    void mapsApplyStyle(const char *style, bool persist); // set style + per-style .format/.url wiring
    lv_obj_t *maps_gear_btn = nullptr;
    lv_obj_t *maps_style_ovl = nullptr;
    void openMapDownload(void);   // capture the current viewport as the download box + show the screen
    void mapdlUpdateEstimate(void);
    void mapdlStart(void);
    void mapdlStop(bool finished);
    void mapdlPump(void);                       // one WiFi/fetch step per timer tick (UI task = SD-safe)
    uint32_t mapdlCountTiles(uint8_t zmin, uint8_t zmax) const;
    lv_obj_t *mapdl_screen = nullptr;
    lv_obj_t *mapdl_status = nullptr;   // big status line ("Connecting…" / "1234 / 5220 tiles")
    lv_obj_t *mapdl_info = nullptr;     // detail text
    lv_obj_t *mapdl_btn = nullptr;      // Start / Stop
    lv_obj_t *mapdl_btn_lbl = nullptr;
    lv_obj_t *mapdl_use_btn = nullptr;  // "Use this map now" (shown when done)
    lv_obj_t *mapdl_zmin_dd = nullptr;  // detail range dropdowns
    lv_obj_t *mapdl_zmax_dd = nullptr;
    lv_timer_t *mapdl_timer = nullptr;  // the pump; independent of which screen is showing
    bool mapdl_running = false;
    bool mapdl_own_wifi = false;        // we brought WiFi up -> we take it down
    bool mapdl_wifi_up = false;
    float mapdl_latN = 0, mapdl_latS = 0, mapdl_lonW = 0, mapdl_lonE = 0; // captured viewport box
    uint8_t mapdl_zmin = 10, mapdl_zmax = 15;
    uint8_t mapdl_z = 0;                            // zoom level currently being fetched
    uint32_t mapdl_x = 0, mapdl_y = 0;              // next tile cursor within mapdl_z
    uint32_t mapdl_total = 0, mapdl_done = 0, mapdl_failed = 0, mapdl_skipped = 0;
    uint32_t mapdl_deadline = 0;                    // WiFi association timeout
    // mesh kill switch + a settings screen reachable from the grid
    void createSettingsScreen(void);
    void openSettings(void);
    void setMeshEnabled(bool on);
    void setGpsEnabled(bool on);             // Settings: GPS on (= always searching) / off (saves battery)
    lv_obj_t *gps_switch = nullptr;          // the on/off switch on the settings screen
    void cycleLocPrecision(void);            // Settings: how exactly others see you (Exact/Rough/Off)
    void updateLocPrecisionLabel(void);      // refresh that button's label from the channel config
    lv_obj_t *loc_precision_label = nullptr; // "Share location" cycle-button label
    lv_obj_t *timeout_btn_label = nullptr;   // "Screen timeout" cycle-button label
    lv_obj_t *mute_switch = nullptr;         // "Sound" on/off (mutes game/timer beeps)
    void cycleScreenTimeout(void);           // advance to the next timeout choice + persist
    void updateTimeoutBtnLabel(void);        // refresh the cycle-button text
    bool gpsEnabled = true;                  // single source of truth for the GPS toggle
    bool meshEnabled = true;                 // single source of truth (toggle + status icon read this)
    lv_obj_t *settings_screen = nullptr;
    lv_obj_t *mesh_status_icon = nullptr;    // dot on the grid: green = live, gray = off
    lv_obj_t *mesh_switch = nullptr;         // the on/off switch on the settings screen
    // Settings: WiFi (SSID + password typed on-device, status/IP shown live)
    lv_obj_t *wifi_switch = nullptr;         // on/off; on = connect to the saved network
    lv_obj_t *wifi_net_btn_lbl = nullptr;    // shows the saved network name on the "Network" button
    lv_obj_t *wifi_status_label = nullptr;   // "Connected 192.168.x.x" / "Connecting…" / "Off"
    lv_obj_t *wifi_ovl = nullptr;            // SSID/password entry overlay (physical-keyboard textarea)
    lv_obj_t *wifi_ta = nullptr;
    lv_timer_t *wifi_entry_guard = nullptr;  // keeps the physical keyboard aimed at the textarea
    bool wifi_entry_psk = false;             // which field the overlay is editing
    void wifiEntryCommit(void);              // save the typed value (Enter or OK) + close
    lv_timer_t *wifi_status_timer = nullptr; // refreshes the status line while Settings is open
    void wifiEntryPrompt(bool psk);          // keyboard overlay for the network name / password
    void closeWifiEntry(void);
    void wifiApplyEnabled(bool enable);      // write network config + apply (reboots to connect)
    void updateWifiStatus(void);             // status line from the TDeckWifi bridge
    void updateWifiNetLabel(void);           // network-name button label from the saved SSID
    lv_obj_t *wifi_scan_ovl = nullptr;       // "choose a network" overlay (scan results)
    lv_obj_t *wifi_scan_list = nullptr;      // list of found networks inside that overlay
    lv_obj_t *wifi_scan_status = nullptr;    // "Scanning…" / "No networks" line
    lv_timer_t *wifi_scan_timer = nullptr;   // polls the async scan for completion
    char wifi_scan_ssids[16][33] = {};       // de-duplicated results backing the list rows
    void wifiScanOpen(void);                 // start a scan + show the chooser (tap Network)
    void wifiScanClose(void);
    void wifiScanPoll(void);                 // fill the list once the scan finishes
    // File Share (FTP): on-demand screen that brings WiFi up, serves the SD over FTP, drops
    // WiFi on exit. See TDeckFtp.cpp / TDeckWifi.cpp on-demand bridge fns.
    lv_obj_t *fileshare_screen = nullptr;
    lv_obj_t *fileshare_status_label = nullptr; // "Connecting…" / "Ready" line
    lv_obj_t *fileshare_info_label = nullptr;   // the ftp:// address + login once connected
    lv_timer_t *fileshare_timer = nullptr;      // pumps the FTP server + drives the connect states
    bool fileshare_ftp_up = false;              // FTP server started (WiFi connected)
    bool fileshare_own_wifi = false;            // WE brought WiFi up (so WE drop it on exit)
    uint32_t fileshare_deadline = 0;            // lv_tick by which we give up waiting for WiFi
    void openFileShare(void);
    void closeFileShare(void); // stop FTP + power WiFi back down; safe to call twice
    // top-bar battery % (fed by updateMetrics for the own node) + paged 2x3 app grid
    lv_obj_t *launcher_battery_label = nullptr;
    lv_obj_t *launcher_mem_label = nullptr;  // diagnostic: free RAM + lowest-since-boot
    lv_obj_t *launcher_unread_label = nullptr; // top-bar unread-message count, next to "mesh"
    lv_obj_t *launcher_pager = nullptr;      // horizontal snap-scroll container of app pages
    lv_obj_t *launcher_dots = nullptr;       // page indicator dots (only if >1 page)
    int launcherBatPct = -1;                 // last known battery %, -1 = unknown yet
    bool launcherBatPlugged = false;         // charging / on external power
    void updateLauncherBattery(void);        // refresh the top-bar % label

    // ---- trackball double-click gesture: Home / lock / wake+unlock ----
    // From an app -> Home. On Home -> lock (black + PIN). Asleep -> wake. Locked -> PIN pad.
    enum TDeckLockState { LOCK_NONE, LOCK_DARK, LOCK_ENTRY };
    TDeckLockState lockState = LOCK_NONE;
    void handleHomeGesture(void);            // runs on every trackball double-click
    void lockDevice(void);                   // black out the screen + require the PIN
    void showLockPad(bool setMode);          // PIN keypad — unlock, or (setMode) choose a new PIN
    void startCalibrationFromLock(void);     // Alt+C from the pad: run touch calibration, then re-lock
    void submitLockPad(void);                // OK pressed on the pad
    void updateLockDisplay(void);            // refresh the masked digits label
    uint32_t effectiveLockPin(void);         // stored PIN, or the 7272 default when unset
    static void lockpad_event(lv_event_t *e);
    lv_obj_t *lockpad_screen = nullptr;
    lv_obj_t *lock_digits_label = nullptr;
    lv_obj_t *lock_title_label = nullptr;
    lv_obj_t *lockpad_unread_label = nullptr; // top-left unread count, mirrors the launcher's
    char lockDigits[9] = {};                 // digits typed on the pad (max 8)
    uint8_t lockLen = 0;
    bool lockSetMode = false;                // true = pad is choosing a NEW pin

    // ---- Files app: browse SD card + internal flash (capped listing — the SD holds ~600k map tiles) ----
    // (openFilesAction, the launcher-tile trampoline, is declared public up top)
    enum FilesVol : uint8_t { VOL_ROOT, VOL_SD, VOL_FLASH, VOL_TRASH }; // ROOT = storage picker
    void openFiles(void);
    void filesRefresh(void);                 // (re)build the list for filesVol/filesPath
    void filesUpdateActionBar(void);         // show/hide the Copy / Paste-here / X / Trash buttons
    void filesCopyNow(void);                 // paste: copy the armed source into filesPath
    void filesTrashSelected(void);           // move the selected file into its volume's /.trash
    void filesRestoreSelected(void);         // trash view: move a file back where it came from
    void filesDeleteForever(void);           // trash view: permanently remove (two-tap confirmed)
    void filesNewFolderPrompt(void);         // ask for a name, then make a folder in filesPath
    void filesNewFolderCreate(const char *name);
    void closeNewFolder(void);               // dismiss the folder-name overlay
    uint64_t filesFreeBytes(bool onSd);      // CACHED free space — measuring the SD card free
    uint64_t filesFreeCache[2] = {0, 0};     // space scans its whole allocation table (seconds
    bool filesFreeKnown[2] = {false, false}; // on a big card), so never do it per navigation
    lv_obj_t *files_screen = nullptr;
    lv_obj_t *files_list = nullptr;
    lv_obj_t *files_path_label = nullptr;
    lv_obj_t *files_free_label = nullptr;    // free space of the volume being browsed
    lv_obj_t *files_copy_btn = nullptr;      // Copy (file selected) / Paste here (copy armed)
    lv_obj_t *files_copy_lbl = nullptr;
    lv_obj_t *files_cancel_btn = nullptr;    // X — disarm a pending copy
    lv_obj_t *files_del_btn = nullptr;       // Delete (files only, two-tap confirm)
    lv_obj_t *files_del_lbl = nullptr;
    bool filesDelConfirm = false;            // first Delete tap happened; next tap really deletes
    lv_obj_t *files_sel_row = nullptr;       // highlighted row of the selected file
    FilesVol filesVol = VOL_ROOT;
    char filesPath[192] = "/";               // current directory within filesVol
    char filesSelName[64] = {};              // selected file's name ("" = none)
    bool filesSelIsDir = false;              // the selection is a folder, not a file
    bool filesSuppressClick = false;         // long-press selected a row; swallow the release-click
    bool filesCopyArmed = false;             // a source is armed, waiting for Paste here
    char copySrcPath[256] = {};              // armed source (full path within its volume)
    FilesVol copySrcVol = VOL_ROOT;
    static constexpr int kFilesMaxEntries = 64;
    char filesNames[kFilesMaxEntries][64];   // entry names backing the list buttons
    bool filesNameIsDir[kFilesMaxEntries] = {}; // parallel to filesNames: is each entry a folder?
    lv_obj_t *files_newfolder_btn = nullptr; // "+ Folder" (shown when nothing is selected)
    lv_obj_t *files_newfolder_lbl = nullptr;
    lv_obj_t *files_newfolder_ovl = nullptr; // folder-name entry overlay (textarea + keyboard)
    lv_obj_t *files_newfolder_ta = nullptr;
    void ui_set_active(lv_obj_t *b, lv_obj_t *p, lv_obj_t *tp);
    void showKeyboard(lv_obj_t *textArea);
    void hideKeyboard(lv_obj_t *panel);
    lv_obj_t *showQrCode(lv_obj_t *parent, const char *data);

    void enablePanel(lv_obj_t *panel);
    void disablePanel(lv_obj_t *panel);
    void setGroupFocus(lv_obj_t *panel);
    void setInputGroup(void);
    void setInputButtonLabel(void);
    void updateGroupChannel(uint8_t chId);

    void backup(uint32_t option);
    void restore(uint32_t option);

    void scanSignal(uint32_t scanNo);
    void handleTraceRouteResponse(const meshtastic_Routing &routing);
    void addNodeToTraceRoute(uint32_t nodeNum, lv_obj_t *panel);
    void purgeNode(uint32_t nodeNum);
    void removeSpinner(void);
    void packetDetected(const meshtastic_MeshPacket &p);
    void writePacketLog(const meshtastic_MeshPacket &p);
    void updateStatistics(const meshtastic_MeshPacket &p);
    void updateSignalStrength(int32_t rssi, float snr);
    int32_t signalStrength2Percent(int32_t rx_rssi, float rx_snr);

    uint32_t preset2val(meshtastic_Config_LoRaConfig_ModemPreset preset);
    meshtastic_Config_LoRaConfig_ModemPreset val2preset(uint32_t val);
    uint32_t role2val(meshtastic_Config_DeviceConfig_Role role);
    meshtastic_Config_DeviceConfig_Role val2role(uint32_t val);
    uint32_t language2val(meshtastic_Language lang);
    meshtastic_Language val2language(uint32_t val);
    void setLocale(meshtastic_Language lang);
    void setLanguage(meshtastic_Language lang);
    void setTimeout(uint32_t timeout);
    void setBrightness(uint32_t brightness);
    void setTheme(uint32_t theme);
    void storeNodeOptions(void);
    void eraseChat(uint32_t channelOrNode);
    void clearChatHistory(void);
    void showLoRaFrequency(const meshtastic_Config_LoRaConfig &cfg);
    void setBellText(bool banner, bool sound);
    void setChannelName(const meshtastic_Channel &ch);
    uint32_t timestamp(char *buf, uint32_t time, bool update);
    void updateLocationMap(uint32_t objects);

    // response callbacks
    void onTextMessageCallback(const ResponseHandler::Request &, ResponseHandler::EventType, int32_t);
    void onPositionCallback(const ResponseHandler::Request &, ResponseHandler::EventType, int32_t);
    void onTracerouteCallback(const ResponseHandler::Request &, ResponseHandler::EventType, int32_t);

    // lvgl timer callbacks
    static void timer_event_reboot(lv_timer_t *timer);
    static void timer_event_shutdown(lv_timer_t *timer);
    static void timer_event_programming_mode(lv_timer_t *timer);

    // lvgl event callbacks
    static void ui_event_LogoButton(lv_event_t *e);
    static void ui_event_BluetoothButton(lv_event_t *e);

    // static void ui_event_HomeButton(lv_event_t * e);
    static void ui_event_NodesButton(lv_event_t *e);
    static void ui_event_GroupsButton(lv_event_t *e);
    static void ui_event_MessagesButton(lv_event_t *e);
    static void ui_event_MapButton(lv_event_t *e);
    static void ui_event_SettingsButton(lv_event_t *e);

    static void ui_event_NodeButton(lv_event_t *e);
    static void ui_event_ChannelButton(lv_event_t *e);
    static void ui_event_ChatButton(lv_event_t *e);
    static void ui_event_ChatDelButton(lv_event_t *e);
    static void ui_event_MsgPopupButton(lv_event_t *e);
    static void ui_event_MsgRestoreButton(lv_event_t *e);
    static void ui_event_AlertButton(lv_event_t *e);

    // Home screen
    static void ui_event_EnvelopeButton(lv_event_t *e);
    static void ui_event_OnlineNodesButton(lv_event_t *e);
    static void ui_event_TimeButton(lv_event_t *e);
    static void ui_event_LoRaButton(lv_event_t *e);
    static void ui_event_BellButton(lv_event_t *e);
    static void ui_event_LocationButton(lv_event_t *e);
    static void ui_event_WLANButton(lv_event_t *e);
    static void ui_event_MQTTButton(lv_event_t *e);
    static void ui_event_SDCardButton(lv_event_t *e);
    static void ui_event_MemoryButton(lv_event_t *e);
    static void ui_event_QrButton(lv_event_t *e);
    static void ui_event_CancelQrButton(lv_event_t *e);

    // blank screen
    static void ui_event_BlankScreenButton(lv_event_t *e);

    static void ui_event_KeyboardButton(lv_event_t *e);
    static void ui_event_Keyboard(lv_event_t *e);

    static void ui_event_message_ready(lv_event_t *e);

    static void ui_event_user_button(lv_event_t *e);
    static void ui_event_role_button(lv_event_t *e);
    static void ui_event_region_button(lv_event_t *e);
    static void ui_event_preset_button(lv_event_t *e);
    static void ui_event_wifi_button(lv_event_t *e);
    static void ui_event_language_button(lv_event_t *e);
    static void ui_event_channel_button(lv_event_t *e);
    static void ui_event_brightness_button(lv_event_t *e);
    static void ui_event_theme_button(lv_event_t *e);
    static void ui_event_calibration_button(lv_event_t *e);
    static void ui_event_timeout_button(lv_event_t *e);
    static void ui_event_screen_lock_button(lv_event_t *e);
    static void ui_event_input_button(lv_event_t *e);
    static void ui_event_alert_button(lv_event_t *e);
    static void ui_event_backup_button(lv_event_t *e);
    static void ui_event_reset_button(lv_event_t *e);
    static void ui_event_reboot_button(lv_event_t *e);
    static void ui_event_device_reboot_button(lv_event_t *e);
    static void ui_event_device_progmode_button(lv_event_t *e);
    static void ui_event_device_shutdown_button(lv_event_t *e);
    static void ui_event_device_cancel_button(lv_event_t *e);
    static void ui_event_shutdown_button(lv_event_t *e);
    static void ui_event_modify_channel(lv_event_t *e);
    static void ui_event_delete_channel(lv_event_t *e);
    static void ui_event_generate_psk(lv_event_t *e);
    static void ui_event_qr_code(lv_event_t *e);

    static void ui_event_screen_timeout_slider(lv_event_t *e);
    static void ui_event_brightness_slider(lv_event_t *e);
    static void ui_event_frequency_slot_slider(lv_event_t *e);
    static void ui_event_modem_preset_dropdown(lv_event_t *e);
    static void ui_event_setup_region_dropdown(lv_event_t *e);
    static void ui_event_map_style_dropdown(lv_event_t *e);
    static void ui_event_map_url_dropdown(lv_event_t *e);

    static void ui_event_calibration_screen_loaded(lv_event_t *e);

    static void ui_event_mesh_detector(lv_event_t *e);
    static void ui_event_mesh_detector_start(lv_event_t *e);
    static void ui_event_signal_scanner(lv_event_t *e);
    static void ui_event_signal_scanner_node(lv_event_t *e);
    static void ui_event_signal_scanner_start(lv_event_t *e);
    static void ui_event_trace_route(lv_event_t *e);
    static void ui_event_trace_route_to(lv_event_t *e);
    static void ui_event_trace_route_start(lv_event_t *e);
    static void ui_event_trace_route_node(lv_event_t *e);
    static void ui_event_node_details(lv_event_t *e);
    static void ui_event_statistics(lv_event_t *e);
    static void ui_event_packet_log(lv_event_t *e);

    static void ui_event_pin_screen_button(lv_event_t *e);
    static void ui_event_statistics_table(lv_event_t *e);

    static void ui_event_ok(lv_event_t *e);
    static void ui_event_cancel(lv_event_t *e);
    static void ui_event_backup_restore_radio_button(lv_event_t *e);

    // map navigation
    static void ui_screen_event_cb(lv_event_t *e);
    static void ui_event_arrow(lv_event_t *e);
    static void ui_event_navHome(lv_event_t *e);
    static void ui_event_zoomSlider(lv_event_t *e);
    static void ui_event_zoomIn(lv_event_t *e);
    static void ui_event_zoomOut(lv_event_t *e);
    static void ui_event_lockGps(lv_event_t *e);
    static void ui_event_mapBrightnessSlider(lv_event_t *e);
    static void ui_event_mapContrastSlider(lv_event_t *e);
    static void ui_event_mapNodeButton(lv_event_t *e);
    static void ui_event_chatNodeButton(lv_event_t *e);
    static void ui_event_positionButton(lv_event_t *e);

    // animations
    static void ui_anim_node_panel_cb(void *var, int32_t v);
    static void ui_anim_radar_cb(void *var, int32_t r);

    lv_obj_t *activeButton = nullptr;
    lv_obj_t *activePanel = nullptr;
    lv_obj_t *activeTopPanel = nullptr;
    lv_obj_t *activeMsgContainer = nullptr;
    lv_obj_t *activeWidget = nullptr;
    lv_obj_t *activeTextInput = nullptr;
    lv_group_t *input_group = nullptr;

    enum BasicSettings activeSettings = eNone; // active settings menu (used to disable other button presses)

    static TFTView_320x240 *gui;                          // singleton pattern
    bool screensInitialised;                              // true if init_screens is completed
    uint32_t nodesFiltered;                               // no. hidden nodes in node list
    bool nodesChanged;                                    // true if nodes changed (added or purged)
    bool processingFilter;                                // indicates that filtering is ongoing
    bool packetLogEnabled;                                // display received packets
    bool detectorRunning;                                 // meshDetector is active
    bool cardDetected;                                    // SD has been detected
    bool formatSD;                                        // offer to format SD card
    uint16_t buttonSize;                                  // size of group/chat buttons in pixels
    uint16_t statisticTableRows;                          // number of rows in statistics table
    uint16_t packetCounter;                               // number of packets in packet log
    time_t lastrun60, lastrun10, lastrun5, lastrun1;      // timers for task loop
    time_t actTime, uptime, lastHeard;                    // actual time and uptime; time last heard a node
    bool hasPosition;                                     // if our position is known
    int32_t myLatitude, myLongitude;                      // our current position as reported by firmware
    void *topNodeLL;                                      // pointer to topmost button in group ll
    uint32_t scans;                                       // scanner counter
    lv_anim_t radar;                                      // radar animation
    static uint32_t currentNode;                          // current selected node
    static lv_obj_t *currentPanel;                        // current selected node panel
    static lv_obj_t *spinnerButton;                       // start button animation
    static time_t startTime;                              // time when start button was pressed
    static uint32_t pinKeys;                              // number of keys pressed (lock screen)
    static bool screenLocked;                             // screen lock active
    static bool screenUnlockRequest;                      // screen unlock request (via button)
    uint32_t selectedHops;                                // remember selected choice
    bool chooseNodeSignalScanner;                         // chose a target node for signal scanner
    bool chooseNodeTraceRoute;                            // chose a target node for trace route
    char old_val1_scratch[64], old_val2_scratch[64];      // temporary scratch buffers for settings strings
    std::array<lv_obj_t *, c_max_channels> ch_label;      // indexable label list for settings
    meshtastic_Channel *channel_scratch;                  // temporary scratch copy of channel db
    lv_obj_t *qr;                                         // qr code
    MapPanel *map = nullptr;                              // map
    std::unordered_map<uint32_t, lv_obj_t *> nodeObjects; // nodeObjects displayed on map
    // extended default device profile struct with additional required data
    struct meshtastic_DeviceProfile_ext : meshtastic_DeviceProfile {
        meshtastic_User user;
        meshtastic_Channel channel[c_max_channels]; // storage of channel info
        meshtastic_DeviceUIConfig uiConfig;         // storage of persistent UI data
    };

    // additional local ui data (non-persistent)
    struct meshtastic_DeviceProfile_full : meshtastic_DeviceProfile_ext {
        bool silent;                                        // sound silenced
        meshtastic_DeviceConnectionStatus connectionStatus; // wifi/bluetooth/ethernet
    };

    meshtastic_DeviceProfile_full db{}; // full copy of the node's configuration db (except nodeinfos) plus ui data
};