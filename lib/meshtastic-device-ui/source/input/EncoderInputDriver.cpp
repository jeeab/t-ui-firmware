#ifdef INPUTDRIVER_ENCODER_TYPE

#include "input/EncoderInputDriver.h"
#include "Arduino.h"
#include "util/ILog.h"

volatile EncoderInputDriver::EncoderActionType EncoderInputDriver::action = TB_ACTION_NONE;

// Set on a trackball double-click; the launcher polls this to run the Home/lock/wake gesture.
volatile bool tb_home_request = false;

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
        bool btnDown = !digitalRead(INPUTDRIVER_ENCODER_BTN);
        if (action == TB_ACTION_NONE && btnDown && !btnWasDown) {
            action = TB_ACTION_PRESSED;
        }
        btnWasDown = btnDown;
#endif
        // slow down repeating key to max. four events per second
        // the button is an exception for LONG_PRESSED monitoring
        if (action != TB_ACTION_NONE && (action == TB_ACTION_PRESSED || millis() > lastPressed + 250)) {
            if (action == TB_ACTION_PRESSED) {
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
            // Trackball ROLL is intentionally inert: all navigation/selection is by touch.
            // (Rolling used to shift launcher pages and move the focus highlight — Jake asked
            // for it to do nothing. UP/DOWN/LEFT/RIGHT are consumed below with no output; only
            // the button double-click above still drives Home / lock / wake.)

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