#ifdef INPUTDRIVER_ENCODER_TYPE

#include "input/EncoderInputDriver.h"
#include "Arduino.h"
#include "util/ILog.h"

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
        static uint32_t lastClickMs = 0; // for trackball double-click -> Home

        data->key = 0;
        data->enc_diff = 0;
        data->state = LV_INDEV_STATE_RELEASED;

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
            } else if (action == TB_ACTION_PRESSED) {
                // Trackball click never selects (selection is by touch). A press only feeds
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
            // Roll stays inert, and the stored flags are deliberately NOT consulted any more.
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