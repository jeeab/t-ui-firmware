// -----------------------------------------------------------------------------
// TDeckBeep — firmware-side bridge letting the device-ui set the audio output
// gain (loudness) across the UI/firmware boundary, so the timer alarm can play
// much louder than the gentle default. Same linker-resolved pattern as
// TDeckMeshSwitch / TDeckGpsBridge / TDeckMemInfo.
// -----------------------------------------------------------------------------
#include "main.h"

#ifdef HAS_I2S
#include "AudioThread.h"
#endif

// g = 0.0..1.0 (default output gain is 0.2). The timer sets ~1.0 for a loud
// alarm, then restores 0.2 afterwards. No-op if there's no audio thread.
extern "C" void tdeck_beep_gain(float g)
{
#ifdef HAS_I2S
    if (audioThread)
        audioThread->setGain(g);
#else
    (void)g;
#endif
}
