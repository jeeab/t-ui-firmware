#ifdef INPUTDRIVER_ENCODER_TYPE

#include "input/EncoderInputDriver.h"
#include "Arduino.h"
#include "util/ILog.h"
#include <esp_heap_caps.h>

volatile EncoderInputDriver::EncoderActionType EncoderInputDriver::action = TB_ACTION_NONE;

// Set on a trackball double-click; the launcher polls this to run the Home/lock/wake gesture.
volatile bool tb_home_request = false;

// Rolling the trackball up or down asks to move a whole ROW, not one item: -1 up, +1 down.
// The launcher polls this, because only it knows how wide the grid is (3 across) and that a
// plain list should still move one line at a time. Left/right are ordinary PREV/NEXT keys and
// need none of this.
volatile int tb_nav_rows = 0;

// Settings -> Trackball navigation (src/TDeckTrackball.cpp). Default OFF, so a device that
// has never touched the setting behaves exactly as before: rolls do nothing at all.
extern "C" bool tdeck_trackball_nav_enabled(void);
// Settings -> Trackball click selects. Separate switch; HOLDING for Home is always on.
extern "C" bool tdeck_trackball_click_enabled(void);
// True while the PIN screen is up - see TFTView_320x240.cpp.
extern "C" bool tdeck_lockscreen_active(void);

// When true the screen is dark/locked: trackball *rolls* are swallowed so they can't wake it
// (only the double-click below wakes). Defined in TFTView_320x240.cpp.
extern volatile bool tdeck_input_gated;
// Settings -> Trackball cursor, and its speed (src/TDeckTrackball.cpp). Default off.
extern "C" bool tdeck_trackball_nav_enabled(void);
extern "C" int tdeck_trackball_speed(void);
extern "C" int tdeck_trackball_style(void); // 0 = see-through, 1 = solid
// "Go back" - the same request the keyboard's erase key raises (TFTView_320x240.cpp).
extern volatile bool tdeck_back_request;
// The lock screen kept lit: rolling or clicking brightens it, it does not unlock.
extern "C" bool tdeck_stayon_active(void);
extern volatile bool tdeck_stayon_boost_request;

// ---- the trackball cursor -------------------------------------------------------------
// A real LVGL pointer, reported exactly like the touchscreen, so a click lands on whatever is
// underneath it - a tile, the map, a game, a Lua app - with nothing needing to opt in.
static lv_indev_t *tbPointer = nullptr;
static lv_obj_t *tbCursorObj = nullptr;
static int16_t tbX = 160, tbY = 120; // starts in the middle
static bool tbTapNow = false;        // deliver one press on the next read
static uint32_t tbLastMoveMs = 0;
static uint32_t tbLastStepMs = 0;
static const uint32_t kCursorIdleHideMs = 6000;

static void tbCursorShow(bool on)
{
    if (!tbCursorObj)
        return;
    if (on)
        lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN);
}

// ---- the arrow ------------------------------------------------------------------------
// ⛔ ONE OBJECT, NOT A STACK OF BARS. lv_indev_set_cursor() moves a single object to follow
// the pointer, and it moves on every roll step - an arrow assembled from little rectangles
// would be ~40 objects to reposition and redraw each time. A canvas is one object with its
// own pixel buffer: drawn once, then LVGL only moves it. It also gives real per-pixel alpha.
//
// THE SHAPE IS A POLYGON, SCAN-FILLED - not a table of row spans. Jake's reference picture
// (2026-09-20) is the classic pointer drawn as an OUTLINE, hollow in the middle, which on a
// 320px screen is the best of both: as easy to find as a solid arrow, and it hides nothing,
// because whatever you are about to click shows through it. Hand-typed spans made the first
// attempt look like a staircase; a polygon keeps the long edges straight at any scale.
//
// Tip at (0,0), down the left edge, out to the tail point, back up, and the long diagonal
// home. Checked in the PC renderer against his picture before it came anywhere near here.
static const int kArrowPts[7][2] = {{0, 0}, {0, 19}, {5, 15}, {8, 22}, {10, 21}, {7, 15}, {12, 14}};
static const int kArrowScale = 2;
static const int kArrowW = 13 * kArrowScale + 2;
static const int kArrowH = 23 * kArrowScale + 2;

// Even-odd scanline test, sampled at pixel centres.
static bool arrowInside(double x, double y)
{
    bool in = false;
    for (int i = 0, j = 6; i < 7; j = i++) {
        const double xi = kArrowPts[i][0], yi = kArrowPts[i][1];
        const double xj = kArrowPts[j][0], yj = kArrowPts[j][1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

static bool arrowAt(int px, int py)
{
    return arrowInside((px - 1 + 0.5) / kArrowScale, (py - 1 + 0.5) / kArrowScale);
}

// Repaint for the current style. Called at init and whenever Settings changes it, so the
// change shows without a reboot. style 0 = outline (hollow), 1 = solid.
static void tbArrowPaint(void)
{
    if (!tbCursorObj)
        return;
    const bool solid = (tdeck_trackball_style() == 1);
    lv_canvas_fill_bg(tbCursorObj, lv_color_hex(0x000000), LV_OPA_TRANSP);
    for (int y = 0; y < kArrowH; y++) {
        for (int x = 0; x < kArrowW; x++) {
            const bool me = arrowAt(x, y);
            bool rim = false, touching = false;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy)
                        continue;
                    const bool n = arrowAt(x + dx, y + dy);
                    if (me && !n)
                        rim = true;
                    if (!me && n)
                        touching = true;
                }
            if (me && (rim || solid)) {
                // The rim is always fully opaque white. That is what keeps the arrow readable
                // over a bright icon - fading it is exactly what sank an earlier translucent
                // cursor, which vanished over the red Alerts bell.
                lv_canvas_set_px(tbCursorObj, x, y, lv_color_hex(0xffffff), rim ? LV_OPA_COVER : 150);
            } else if (!me && touching) {
                // A dark halo just outside, so the white rim still shows on a white map tile.
                lv_canvas_set_px(tbCursorObj, x, y, lv_color_hex(0x000000), 200);
            }
        }
    }
}

extern "C" void tdeck_trackball_cursor_restyle(void)
{
    tbArrowPaint();
}

// Rolled into an edge with nowhere left to go: that overflow becomes scroll. Consumed by
// the UI timer (tdeck_trackball_take_scroll), never acted on here.
static int tbScrollDx = 0, tbScrollDy = 0;

// One roll step. It accelerates, so a fast roll crosses the screen while a single nudge moves
// a few pixels; the Settings speed scales the whole thing.
static void tbMove(int dx, int dy)
{
    const uint32_t now = millis();
    const uint32_t gap = now - tbLastStepMs;
    tbLastStepMs = now;
    int step = (gap < 70) ? 16 : (gap < 160 ? 8 : 4);
    switch (tdeck_trackball_speed()) {
    case 0: step = (step + 1) / 2; break; // slow
    case 2: step = step * 2; break;       // fast
    default: break;                       // normal
    }

    int nx = tbX + dx * step, ny = tbY + dy * step;
    // ⚠️ Only the part of the roll that CANNOT move the cursor becomes scroll. Sitting at an
    // edge therefore does nothing; you have to keep rolling into it. And the page moves by
    // however much you rolled, so it stops dead when you do.
    if (nx < 2) {
        tbScrollDx += nx - 2;
        nx = 2;
    } else if (nx > 317) {
        tbScrollDx += nx - 317;
        nx = 317;
    }
    if (ny < 2) {
        tbScrollDy += ny - 2;
        ny = 2;
    } else if (ny > 237) {
        tbScrollDy += ny - 237;
        ny = 237;
    }
    tbX = (int16_t)nx;
    tbY = (int16_t)ny;
    tbLastMoveMs = now;
    tbCursorShow(true);
}

// Hand the pending scroll to the caller and clear it. Returns false when there is nothing,
// which is almost always.
extern "C" bool tdeck_trackball_take_scroll(int *dx, int *dy)
{
    if (!tbScrollDx && !tbScrollDy)
        return false;
    if (dx)
        *dx = tbScrollDx;
    if (dy)
        *dy = tbScrollDy;
    tbScrollDx = 0;
    tbScrollDy = 0;
    return true;
}

static void tb_pointer_read(lv_indev_t *, lv_indev_data_t *data)
{
    data->point.x = tbX;
    data->point.y = tbY;
    // One read pressed, then straight back up: a tap. A HELD button must never become a long
    // press on whatever is under the cursor - that would make dragging and scrolling
    // impossible - so the press is always exactly one read long.
    data->state = tbTapNow ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    tbTapNow = false;
    if (tbCursorObj && !lv_obj_has_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN) &&
        millis() - tbLastMoveMs > kCursorIdleHideMs)
        tbCursorShow(false);
}

// Built the first time the cursor is actually wanted, so a device with the setting off never
// creates any of it.
static void tbCursorInit(void)
{
    if (tbPointer)
        return;
    tbPointer = lv_indev_create();
    lv_indev_set_type(tbPointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(tbPointer, tb_pointer_read);

    // PSRAM, allocated once and never freed: the canvas keeps the pointer for the life of
    // the object. 28 x 38 at 4 bytes a pixel is about 4KB.
    static uint8_t *arrowBuf = nullptr;
    if (!arrowBuf)
        arrowBuf = (uint8_t *)heap_caps_malloc(kArrowW * kArrowH * 4 + 64, MALLOC_CAP_SPIRAM);
    if (!arrowBuf)
        return; // no cursor rather than a bad one
    tbCursorObj = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(tbCursorObj, arrowBuf, kArrowW, kArrowH, LV_COLOR_FORMAT_ARGB8888);
    tbArrowPaint();
    lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tbCursorObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_indev_set_cursor(tbPointer, tbCursorObj); // LVGL keeps it on the point for us
    tbCursorShow(false);
}

// Cursor off again: put the pointer away so it cannot hover or be seen.
// Where the cursor is, for the edge-scroll in TFTView. Returns false when there is nothing
// to do - cursor off, never created, or currently hidden - so the caller can bail cheaply.
extern "C" bool tdeck_trackball_cursor_state(int *x, int *y)
{
    if (!tbCursorObj || !tdeck_trackball_nav_enabled())
        return false;
    if (lv_obj_has_flag(tbCursorObj, LV_OBJ_FLAG_HIDDEN))
        return false;
    if (x)
        *x = tbX;
    if (y)
        *y = tbY;
    return true;
}

extern "C" void tdeck_trackball_cursor_off(void)
{
    tbCursorShow(false);
}

EncoderInputDriver::EncoderInputDriver(void) {}

void EncoderInputDriver::init(void)
{
    // trackball or joystick type encoder with four directions
    if (INPUTDRIVER_ENCODER_TYPE == 3) {
#ifdef INPUTDRIVER_ENCODER_LEFT
        pinMode(INPUTDRIVER_ENCODER_LEFT, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_LEFT, intLeftHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_RIGHT
        pinMode(INPUTDRIVER_ENCODER_RIGHT, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_RIGHT, intRightHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_UP
        pinMode(INPUTDRIVER_ENCODER_UP, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_UP, intUpHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_DOWN
        pinMode(INPUTDRIVER_ENCODER_DOWN, INPUT_PULLUP);
        attachInterrupt(INPUTDRIVER_ENCODER_DOWN, intDownHandler, RISING);
#endif
#ifdef INPUTDRIVER_ENCODER_BTN
        pinMode(INPUTDRIVER_ENCODER_BTN, INPUT);
#endif
    }

    encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder, encoder_read);

    if (!inputGroup) {
        inputGroup = lv_group_create();
        lv_group_set_default(inputGroup);
    }
    lv_indev_set_group(encoder, inputGroup);
}

void EncoderInputDriver::encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    // encoder w/o interrupts but read GPIOs directly
    if (INPUTDRIVER_ENCODER_TYPE == 1) {
#ifdef INPUTDRIVER_ENCODER_LEFT
        if (digitalRead(INPUTDRIVER_ENCODER_LEFT))
            data->enc_diff = -1;
#endif
#ifdef INPUTDRIVER_ENCODER_RIGHT
        if (digitalRead(INPUTDRIVER_ENCODER_RIGHT))
            data->enc_diff = 1;
#endif
#ifdef INPUTDRIVER_ENCODER_BTN
        // FIXME: need same logix as below to trigger LONG_PRESSED events
        if (!digitalRead(INPUTDRIVER_ENCODER_BTN)) {
            data->key = LV_KEY_ENTER;
            data->state = LV_INDEV_STATE_PRESSED;
        }
#endif
    }
    // trackball/joystick with additional up/down inputs to control sliders
    else if (INPUTDRIVER_ENCODER_TYPE == 3) {
        static uint32_t prevkey = 0;
        static uint32_t lastPressed = millis();
        static uint32_t lastClickMs = 0;      // for the double-click gesture
        static uint32_t tbClickPendingAt = 0; // a single click waiting out the double window

        data->key = 0;
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;

        // A single click that has now outlived the double-click window is a tap. Resolved here
        // rather than in the block below because it has to fire on a read where nothing at all
        // happened - that is the whole point of waiting.
        if (tbClickPendingAt && millis() - tbClickPendingAt > 320) {
            tbClickPendingAt = 0;
            tbTapNow = true;
        }

#ifdef INPUTDRIVER_ENCODER_BTN
        // Fire PRESSED only on the button's DOWN EDGE (released -> pressed). The button is
        // polled every read, so a normal click stays "down" across several polls; counting
        // each poll as a click made every single click look like a double -> Home. Tracking
        // the edge means one physical click == exactly one PRESSED.
        static bool btnWasDown = false;
        static uint32_t btnDownAt = 0;
        static bool longFired = false;
        bool btnDown = !digitalRead(INPUTDRIVER_ENCODER_BTN);

        // HOLD -> Home, for everyone, in every mode. It is deliberately not a setting: a hold
        // costs nothing to detect (a quick click is unambiguous the moment the button comes back
        // up, unlike a double-click, which you can only recognise by waiting out the whole
        // window), and it guarantees there is always one gesture that gets you out no matter how
        // the two switches are set. Double-click still goes Home too, whenever the click is not
        // being used to select.
        if (btnDown && !btnWasDown) {
            btnDownAt = millis();
            longFired = false;
        } else if (btnDown && !longFired && millis() - btnDownAt > 700) {
            // ⚠️ The cursor suspends this ONLY while the screen is awake. With the cursor on
            // a hold has to keep pressing whatever is being pointed at, or dragging a slider
            // and scrolling a list are impossible - but that reasoning stops dead when the
            // screen is dark, because there is nothing to point at and this is the gesture
            // that gets you back in. Dropping the exception while gated is what stops the
            // device becoming unwakeable; it did, once, and that is why the test is here.
            if (!tdeck_trackball_nav_enabled() || tdeck_input_gated)
                tb_home_request = true; // held -> Home / lock / wake
            longFired = true;
        } else if (!btnDown && btnWasDown && !longFired) {
            // A quick click. What it MEANS is decided in the block below: either a select, or
            // one half of the old double-click gesture.
            action = TB_ACTION_PRESSED;
        }
        btnWasDown = btnDown;
#endif
        // slow down repeating key to max. four events per second
        // the button is an exception for LONG_PRESSED monitoring
        // Kept-lit lock screen: any trackball activity brightens it for three seconds and
        // nothing else. The double-click that goes Home still works, because that is handled
        // above this and sets tb_home_request directly.
        if (action != TB_ACTION_NONE && tdeck_stayon_active()) {
            tdeck_stayon_boost_request = true;
        }
        if (action != TB_ACTION_NONE && (action == TB_ACTION_PRESSED || millis() > lastPressed + 250)) {
            // On the PIN screen the trackball must not select or navigate anything: the group
            // still holds the launcher's tiles, so a click here would press a button behind the
            // lock. Holding for Home is handled above and still works, which is the one gesture
            // that should survive.
            if (tdeck_lockscreen_active()) {
                data->key = 0;
                data->enc_diff = 0;
                data->state = LV_INDEV_STATE_RELEASED;
                tb_nav_rows = 0;
            } else if (false) { // click-to-select: gone with its switch, see the note below
                // Click-to-select only means anything when there is a highlight to select, i.e.
                // when navigation is on. Requiring BOTH matters: navigation is currently disabled
                // (see tdeckRefreshNavGroup), so with only the click switch on the click sent
                // Enter into an empty group - it selected nothing AND it suppressed the
                // double-click that goes Home, leaving no obvious way out of an app. Tying the
                // two together means the old double-click gesture always comes back whenever
                // navigation is not actually running.
                data->key = LV_KEY_ENTER;
                data->state = LV_INDEV_STATE_PRESSED;
            } else if (action == TB_ACTION_PRESSED && tdeck_trackball_nav_enabled() &&
                       !tdeck_input_gated) {
                // Cursor mode, and only while awake. Asleep, a click belongs to the
                // double-click-to-wake gesture in the branch below - the cursor is not on
                // screen and cannot be the thing a click means.
                // Single click presses what the cursor is over; double click goes
                // Back. Telling them apart means waiting out the window before acting on a
                // single one, so it is kept short - 320ms is long enough for a deliberate
                // double and short enough that a single click still feels immediate.
                tbCursorInit();
                const uint32_t nowMs = millis();
                if (lastClickMs && nowMs - lastClickMs < 320) {
                    tdeck_back_request = true; // double -> Back
                    lastClickMs = 0;
                    tbClickPendingAt = 0;      // and cancel the single that was waiting
                } else {
                    lastClickMs = nowMs;
                    tbClickPendingAt = nowMs;  // a tap, unless a second click arrives
                }
            } else if (action == TB_ACTION_PRESSED) {
                // Cursor off: the click never selects (selection is by touch). It only feeds
                // the double-click -> Home gesture, which also wakes the screen if it's asleep.
                uint32_t nowMs = millis();
                // Generous window: two clicks within 1.5s count as a double-click. The stiff
                // trackball in Jake's 3D case makes fast double-clicks hard; single-click has
                // no action so a wide window costs nothing.
                if (nowMs - lastClickMs < 1500) {
                    tb_home_request = true; // second click within window -> Home (+ wake)
                    lastClickMs = 0;        // reset so a 3rd click starts a fresh pair
                } else {
                    lastClickMs = nowMs; // first click: remember it, emit nothing
                }
            }
            // Trackball ROLL is inert by DEFAULT: all navigation/selection is by touch, and
            // UP/DOWN/LEFT/RIGHT are consumed here with no output — only the button double-click
            // above drives Home / lock / wake. That is a deliberate choice, not a dead trackball,
            // but it looks identical to broken hardware from the outside (t-ui-firmware#1), so
            // Settings -> Trackball navigation turns rolling back on for people who want it.
            // Switched on, a roll emits the ordinary LVGL arrow keys, which is what used to move
            // the focus highlight and flip launcher pages.
            // Rolling moves the cursor, when the user has asked for it.
            if (tdeck_trackball_nav_enabled() && action != TB_ACTION_PRESSED &&
                action != TB_ACTION_NONE) {
                tbCursorInit();
                switch (action) {
                case TB_ACTION_LEFT:  tbMove(-1, 0); break;
                case TB_ACTION_RIGHT: tbMove(1, 0);  break;
                case TB_ACTION_UP:    tbMove(0, -1); break;
                case TB_ACTION_DOWN:  tbMove(0, 1);  break;
                default: break;
                }
            }
            // Roll stays inert otherwise, and the stored flags are deliberately NOT consulted.
            // The switches are gone from Settings, so anyone who had turned them on would
            // otherwise be left with a trackball that shuffles focus around the node list and no
            // way to stop it. Hold-for-Home and double-click-for-Home both still work, and
            // neither is a setting.
            else if (false) {
                // PREV/NEXT, not the arrow keys. In LVGL an arrow key is delivered TO the
                // focused widget - it means "act on this thing" - so on a vertical-scrolling
                // screen LVGL turns left/right into a scroll and leaves up/down to a widget
                // that usually ignores them. That is exactly backwards for a trackball, and it
                // is why the first build scrolled Settings with LEFT and RIGHT while up and
                // down did nothing. PREV/NEXT move the FOCUS between widgets, which is what
                // rolling should do, and LVGL scrolls the newly focused item into view for free.
                switch (action) {
                case TB_ACTION_LEFT:
                    data->key = LV_KEY_PREV;
                    break;
                case TB_ACTION_RIGHT:
                    data->key = LV_KEY_NEXT;
                    break;
                case TB_ACTION_UP:
                    tb_nav_rows = -1; // a whole row; the launcher decides how wide that is
                    break;
                case TB_ACTION_DOWN:
                    tb_nav_rows = 1;
                    break;
                default:
                    break;
                }
                if (data->key)
                    data->state = LV_INDEV_STATE_PRESSED;
            }

            // Screen dark/locked: drop any roll movement (the double-click above still
            // sets tb_home_request, which is how the screen gets woken).
            if (tdeck_input_gated) {
                data->key = 0;
                data->enc_diff = 0;
                data->state = LV_INDEV_STATE_RELEASED;
            }

            lastPressed = millis();
            prevkey = data->key;
            action = TB_ACTION_NONE;
        } else {
            // this logic is required for LONG_PRESSED event, see lv_indev.c
            if (prevkey != 0) {
                data->state = LV_INDEV_STATE_RELEASED;
                data->key = prevkey;
                prevkey = 0;
            }
        }
    }
}

void EncoderInputDriver::intPressHandler()
{
    action = TB_ACTION_PRESSED;
}

void EncoderInputDriver::intDownHandler()
{
    action = TB_ACTION_DOWN;
}

void EncoderInputDriver::intUpHandler()
{
    action = TB_ACTION_UP;
}

void EncoderInputDriver::intLeftHandler()
{
    action = TB_ACTION_LEFT;
}

void EncoderInputDriver::intRightHandler()
{
    action = TB_ACTION_RIGHT;
}

#endif