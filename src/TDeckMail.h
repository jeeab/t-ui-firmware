#pragma once

// Gmail over IMAP. See TDeckMail.cpp - step one is connect/authenticate/count only, built first
// to prove certificate verification and the heap before an inbox UI is stacked on top.
//
// Credentials live on the SD card in /gmail.txt (address, then the 16-character app password),
// never compiled into the firmware, so the installer stays shareable.

#ifdef __cplusplus
extern "C" {
#endif

// Ask for an inbox check. false if one is already running. Safe from the UI task: it only
// records the intent; tdeck_mail_service() on the main loop does the work.
bool tdeck_mail_check(void);

// Connect, verify the certificate, read the IMAP greeting, log out. NO credentials involved,
// so it proves the certificate and the heap on their own - and later tells "network/certificate
// is wrong" apart from "login is wrong", which otherwise look identical from the device.
bool tdeck_mail_connect_test(void);

// Drop the cached address/password so /gmail.txt is read again. Called by the setup form after
// it writes the file.
void tdeck_mail_forget_creds(void);

// 0 = still working, 1 = done, -1 = failed (see tdeck_mail_error()).
int tdeck_mail_poll(void);

// Last successful counts; -1 if unknown.
void tdeck_mail_counts(int *total, int *unseen);

// Human-readable reason the last attempt failed. Never contains the password.
const char *tdeck_mail_error(void);

// Called from loop() in main.cpp, NOT from the UI task.
void tdeck_mail_service(void);

#ifdef __cplusplus
}
#endif
