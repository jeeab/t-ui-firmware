// -----------------------------------------------------------------------------
// T-Deck launcher: FTP file-share bridge (firmware side)
//
// Serves the SD card over WiFi so files can be dragged to/from a computer with any
// FTP client (Windows Explorer, macOS Finder, FileZilla). On-demand only: the UI
// brings WiFi up, starts this, shows the address, and stops+drops WiFi on exit — so
// WiFi (and its RAM cost) is only alive while you're actually transferring.
//
// Storage backend is STORAGE_SD (set in variants/esp32s3/t-deck/platformio.ini), which
// uses the Arduino SD.h instance the firmware already mounts via FSCommon. We do NOT
// call SD.begin() here — the card is already mounted; re-initializing it with the wrong
// (default) SPI pins would break it.
//
// HARDWARE RISK TO VERIFY ON-DEVICE: the SD card and the LoRa radio share the SPI bus,
// serialized elsewhere by spiLock. SimpleFTPServer doesn't know about that lock, so a
// transfer running at the same instant as a radio SPI transaction is the thing to watch
// for (corruption / hangs). Test with real transfers before trusting it with important
// files. If it's a problem, the fix is to guard handleFTP()'s SD access with spiLock.
// -----------------------------------------------------------------------------
#include "configuration.h"

#if HAS_WIFI && defined(HAS_SDCARD)
#include <SimpleFTPServer.h>

static FtpServer *ftpSrv = nullptr;
static bool ftpRunning = false;

// Start the FTP server with a login. Safe to call repeatedly (no-op if already running).
extern "C" void tdeck_ftp_start(const char *user, const char *pass)
{
    if (ftpRunning)
        return;
    if (!ftpSrv)
        ftpSrv = new FtpServer();
    ftpSrv->begin(user, pass);
    ftpRunning = true;
}

// Stop the server (called when the File Share screen closes).
extern "C" void tdeck_ftp_stop(void)
{
    if (!ftpRunning)
        return;
    if (ftpSrv)
        ftpSrv->end();
    ftpRunning = false;
}

// Pump the server — must be called often (every UI tick) while running so transfers progress.
extern "C" void tdeck_ftp_service(void)
{
    if (ftpRunning && ftpSrv)
        ftpSrv->handleFTP();
}

extern "C" bool tdeck_ftp_running(void)
{
    return ftpRunning;
}

#else  // no WiFi or no SD on this build — stubs so the UI links cleanly
extern "C" void tdeck_ftp_start(const char *, const char *) {}
extern "C" void tdeck_ftp_stop(void) {}
extern "C" void tdeck_ftp_service(void) {}
extern "C" bool tdeck_ftp_running(void) { return false; }
#endif
