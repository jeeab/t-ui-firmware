// -----------------------------------------------------------------------------
// TDeckPop - the new-message "pop".
//
// Jake, 2026-09-18: "can you add a classic facebook pop notification sound?"
// The sound itself is synthesised, not Facebook's file; see tools/make_pop.py.
//
// THREADING, which is the whole reason this is a file and not two lines:
// a message arrives on the MESH task, and that task must never block. Stalls of
// RadioIf waiting on a lock are what have been rebooting this device - the on-device
// diagnostics recorded ten of them at about 24 seconds each. The existing tone path
// (buzz.cpp playTonesRTTTL) spins on `while (audioThread->isPlaying()) delay(10)`,
// so calling it from there would be actively harmful.
//
// So: notif_add() (or anyone else) calls tdeck_pop_request(), which only sets a flag.
// tdeck_pop_service() runs on the main loop, starts the WAV, and returns immediately;
// AudioThread::runOnce() pumps it to the end. Nothing waits for the sound.
// Same deferred-service shape as TDeckBeep / TDeckGpsBridge / TDeckClockFormat.
// -----------------------------------------------------------------------------
#include "main.h"
#include "mesh/NodeDB.h" // config.device.buzzer_mode

#ifdef HAS_I2S
#include "AudioThread.h"
#include "pop_wav.h"
#endif

static volatile bool s_popPending = false;

// Callable from ANY task. Records intent and nothing else.
extern "C" void tdeck_pop_request(void)
{
    s_popPending = true;
}

// Main loop only.
extern "C" void tdeck_pop_service(void)
{
    if (!s_popPending)
        return;
    s_popPending = false;

#ifdef HAS_I2S
    // One switch governs every sound on the device (Settings > Sound drives Meshtastic's
    // own buzzer_mode - see TDeckBeep.cpp). DISABLED means silent. NOTIFICATIONS_ONLY
    // still pops, because a notification is precisely what this is.
    if (config.device.buzzer_mode == meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED)
        return;
    if (!audioThread)
        return;
    // Don't cut across a ringtone or an alarm that is already sounding.
    if (audioThread->wavPlaying())
        return;
    audioThread->beginWav(kPopWav, kPopWavLen);
#endif
}
