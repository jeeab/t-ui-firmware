#pragma once
#include <stdint.h>

// -----------------------------------------------------------------------------
// Gemini for the colour T-Deck.
//
// Jake: "Could you make a Gemini app for that?" - the Max has had one for a while; this is the
// same idea on the colour device, reusing that version's hard-won parts.
//
// ⛔ THE KEY IS NEVER COMPILED IN. It is read at runtime from /gemini.txt on the SD card, so the
// firmware and the web installer can be handed to anyone without handing over Jake's key. Same
// rule as the Max - see gemini-key-TEMPLATE.txt.
//
// ⚠️ USING THIS DROPS BLUETOOTH. Starting wi-fi on this board calls disableBluetooth() (one
// antenna), and BT stays down until the next reboot. Jake accepted that: "Idea is anyway I won't
// Bluetooth much to my tdeck". Worth saying out loud in the UI too, so nobody loses their phone
// link by surprise.
//
// THREADING: ask() only records a question. service(), called from the main loop, drives the
// state machine and the network - the UI task must never block on a socket. This is the same
// deferred pattern as the map, pins, notes and weather work.
// -----------------------------------------------------------------------------

namespace tdeckgemini
{
enum State {
    IDLE = 0, // nothing asked
    WORKING,  // connecting / sending / waiting
    DONE,     // reply() holds an answer
    FAILED    // statusText() says why
};

void service();               // from the main loop (safe thread)
void ask(const char *prompt); // record a question (safe from the UI task)
int state();
const char *reply();      // the answer, "" until DONE
const char *statusText(); // short progress or error line
bool haveConfig();        // /gemini.txt exists and holds a key
void clear();             // drop the answer, back to IDLE
} // namespace tdeckgemini
