#pragma once

#include <stdbool.h>
#include <stdint.h>

// The chess GAME, kept away from the UI. The engine itself is src/chess/; this owns the current
// position, whose turn it is, and running the search somewhere it cannot freeze the screen.
//
// ⛔ THE SEARCH MUST NOT RUN ON THE UI TASK. That task holds spiLock while it runs, so a
// two-second think would freeze the display AND stall the radio - the exact failure this device
// has a long history with. The UI records that it wants a move; tdeck_chess_service(), called
// from loop() in main.cpp, does the thinking and leaves the answer to be collected.

#ifdef __cplusplus
extern "C" {
#endif

void tdeck_chess_new_game(void);

// Piece on a square, in 0..63 with 0 = a1 and 63 = h8. 0 = empty, otherwise the engine's
// encoding: colour in bit 3, type in bits 0-2 (1 pawn .. 6 king).
uint8_t tdeck_chess_piece_at(int sq64);

// Is this square part of the move being shown (selected square, or the engine's last move)?
bool tdeck_chess_highlighted(int sq64);

// Legal destinations from a square, for showing the player where a piece can go.
int tdeck_chess_moves_from(int sq64, uint8_t *dests, int cap);

// Try the player's move. Returns false if it is not legal, which also makes this the validator.
// Promotion is automatic to a queen - see the note in the .cpp.
bool tdeck_chess_try_move(int from64, int to64);

// Whose turn, and whether a think is in progress.
bool tdeck_chess_player_turn(void);
bool tdeck_chess_thinking(void);

// Ask the engine to move. Returns false if it is already busy or the game is over.
bool tdeck_chess_request_engine(void);

// Take back the last full move (yours and the reply), so one tap undoes what you did.
bool tdeck_chess_undo(void);

// 0 playing, 1 you are mated, 2 engine is mated, 3 stalemate, 4 draw.
int tdeck_chess_result(void);

// One line for the status bar: whose turn, what the engine found, or how it ended.
const char *tdeck_chess_status(void);

int tdeck_chess_level(void);
void tdeck_chess_set_level(int lv);
const char *tdeck_chess_level_name(void);

// Called from loop() in main.cpp, NOT from the UI task.
void tdeck_chess_service(void);

#ifdef __cplusplus
}
#endif
