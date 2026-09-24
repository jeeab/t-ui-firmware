#include "configuration.h"

#include "TDeckChess.h"
#include "chess/chess.h"
#include "chess/search.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// -----------------------------------------------------------------------------------------
// The chess game. Jake, 2026-09-22: "local chess with a chess ai kinda thing with difficulty
// settings up to master".
//
// ⛔ THE SEARCH RUNS HERE, ON THE MAIN LOOP, NEVER ON THE UI TASK. Measured on this chip: the
// engine does ~16-20 thousand positions a second, so Club level is about two seconds of
// thinking. The UI task holds spiLock for as long as it runs, so two seconds of searching there
// would freeze the screen and stall the radio at the same time. The UI sets a flag; this does
// the work and leaves the answer behind.
//
// Even on the main loop a two-second think blocks mesh servicing, so the time budget is kept
// modest and the whole thing is deliberately turn-based - nothing here runs unless the player
// has just moved.
// -----------------------------------------------------------------------------------------

static Board s_board;
static bool s_ready = false;
static volatile bool s_wantMove = false;
static volatile bool s_thinking = false;
static int s_level = CHESS_CLUB;
static char s_status[64] = "Your move";
static uint8_t s_lastFrom = 0xFF, s_lastTo = 0xFF; // the engine's reply, for highlighting
static uint8_t s_selected = 0xFF;                  // set by the UI via tdeck_chess_moves_from

// Undo history. Two plies per entry so one tap takes back a whole exchange.
#define UNDO_MAX 64
static Undo s_undo[UNDO_MAX];
static int s_undoN = 0;

static void *psram(unsigned long n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

static void ensureInit(void)
{
    if (s_ready)
        return;
    // ⭐ EVERYTHING THE ENGINE ALLOCATES GOES TO PSRAM. The transposition table alone is 1MB and
    // the move stack another 96KB; in internal RAM they would take the whole device down. See
    // the note in search.c - this was learned the hard way.
    search_init(psram, 1024UL * 1024UL);
    chess_init(&s_board);
    search_history_clear();
    search_history_push(s_board.hash);
    s_ready = true;
}

#define SQ88(sq64) (((sq64) / 8) * 16 + ((sq64) % 8))
#define SQ64OF(sq88) (((sq88) >> 4) * 8 + ((sq88) & 7))

void tdeck_chess_new_game(void)
{
    ensureInit();
    chess_init(&s_board);
    search_history_clear();
    search_history_push(s_board.hash);
    s_undoN = 0;
    s_lastFrom = s_lastTo = 0xFF;
    s_selected = 0xFF;
    s_wantMove = false;
    strncpy(s_status, "Your move", sizeof(s_status) - 1);
}

uint8_t tdeck_chess_piece_at(int sq64)
{
    ensureInit();
    if (sq64 < 0 || sq64 > 63)
        return 0;
    return s_board.board[SQ88(sq64)];
}

bool tdeck_chess_highlighted(int sq64)
{
    const uint8_t sq = (uint8_t)SQ88(sq64);
    return sq == s_lastFrom || sq == s_lastTo || sq == s_selected;
}

int tdeck_chess_moves_from(int sq64, uint8_t *dests, int cap)
{
    ensureInit();
    s_selected = (uint8_t)SQ88(sq64);
    Move list[MAX_MOVES];
    const int n = chess_gen_moves(&s_board, list);
    int out = 0;
    for (int i = 0; i < n && out < cap; i++)
        if (list[i].from == s_selected)
            dests[out++] = (uint8_t)SQ64OF(list[i].to);
    if (!out)
        s_selected = 0xFF; // nothing to show; do not leave a square lit with no moves
    return out;
}

bool tdeck_chess_try_move(int from64, int to64)
{
    ensureInit();
    if (s_thinking)
        return false;
    const uint8_t f = (uint8_t)SQ88(from64), t = (uint8_t)SQ88(to64);
    Move list[MAX_MOVES];
    const int n = chess_gen_moves(&s_board, list);
    const Move *found = nullptr;
    for (int i = 0; i < n; i++) {
        if (list[i].from != f || list[i].to != t)
            continue;
        // ⭐ AUTO-QUEEN. Four promotion moves share a from/to, so something has to choose. A
        // promotion dialog on a touchscreen this size is a lot of UI for a case that wants a
        // queen more than 95% of the time. Underpromotion is the cost, and it is noted rather
        // than hidden.
        if ((list[i].flags & MOVE_PROMO) && list[i].promo != QUEEN)
            continue;
        found = &list[i];
        break;
    }
    if (!found)
        return false;
    if (s_undoN < UNDO_MAX)
        chess_make(&s_board, found, &s_undo[s_undoN++]);
    else
        return false; // 64 moves of history is plenty; refusing beats losing the ability to undo
    search_history_push(s_board.hash);
    s_lastFrom = f;
    s_lastTo = t;
    s_selected = 0xFF;
    return true;
}

bool tdeck_chess_player_turn(void)
{
    ensureInit();
    return s_board.side == WHITE && !s_thinking;
}

bool tdeck_chess_thinking(void)
{
    return s_thinking;
}

bool tdeck_chess_request_engine(void)
{
    ensureInit();
    if (s_thinking || chess_game_over(&s_board))
        return false;
    s_wantMove = true;
    s_thinking = true;
    strncpy(s_status, "Thinking...", sizeof(s_status) - 1);
    return true;
}

bool tdeck_chess_undo(void)
{
    ensureInit();
    if (s_thinking || s_undoN == 0)
        return false;
    // Take back the engine's reply AND your move, so one tap returns you to where you chose.
    int take = (s_undoN >= 2) ? 2 : 1;
    while (take--) {
        chess_unmake(&s_board, &s_undo[--s_undoN]);
        search_history_pop();
    }
    s_lastFrom = s_lastTo = 0xFF;
    s_selected = 0xFF;
    strncpy(s_status, "Took that back", sizeof(s_status) - 1);
    return true;
}

int tdeck_chess_result(void)
{
    ensureInit();
    const int over = chess_game_over(&s_board);
    if (over == 1)
        return s_board.side == WHITE ? 1 : 2; // whoever is to move is the one mated
    if (over == 2)
        return 3;
    if (over)
        return 4;
    return 0;
}

const char *tdeck_chess_status(void)
{
    return s_status;
}

int tdeck_chess_level(void)
{
    return s_level;
}

void tdeck_chess_set_level(int lv)
{
    if (lv < 0 || lv >= CHESS_LEVELS)
        lv = CHESS_CLUB;
    s_level = lv;
}

const char *tdeck_chess_level_name(void)
{
    return chess_level_name((ChessLevel)s_level);
}

static void setEndStatus(void)
{
    switch (tdeck_chess_result()) {
    case 1: strncpy(s_status, "Checkmate - you lose", sizeof(s_status) - 1); break;
    case 2: strncpy(s_status, "Checkmate - you win", sizeof(s_status) - 1); break;
    case 3: strncpy(s_status, "Stalemate - a draw", sizeof(s_status) - 1); break;
    case 4: strncpy(s_status, "Drawn", sizeof(s_status) - 1); break;
    default: break;
    }
}

void tdeck_chess_service(void)
{
    if (!s_wantMove)
        return;
    s_wantMove = false;
    if (!s_ready) {
        s_thinking = false;
        return;
    }

    Move m;
    // ⚠️ Time budget is capped well below the level's own default. Even here this blocks mesh
    // servicing for its duration, and a handheld that stops relaying packets for eight seconds
    // because someone is playing chess is a bad trade. Measured: ~16-20 knps on this chip, so
    // 2.5s reaches about depth 6.
    const uint32_t budget = 2500;
    if (!search_best_move(&s_board, (ChessLevel)s_level, budget, &m)) {
        s_thinking = false;
        setEndStatus();
        return;
    }
    Undo u;
    if (s_undoN < UNDO_MAX) {
        chess_make(&s_board, &m, &s_undo[s_undoN++]);
        search_history_push(s_board.hash);
        s_lastFrom = m.from;
        s_lastTo = m.to;
    }

    SearchInfo in;
    search_last_info(&in);
    char mv[6];
    chess_move_str(&m, mv);
    if (chess_game_over(&s_board)) {
        setEndStatus();
    } else if (in.mateIn) {
        snprintf(s_status, sizeof(s_status), "%s - mate in %d", mv, in.mateIn > 0 ? in.mateIn : -in.mateIn);
    } else {
        // The score is from the side to move, which after the engine's move is the PLAYER - so
        // a positive number here already means "good for you". Shown that way round on purpose.
        snprintf(s_status, sizeof(s_status), "%s  depth %d  %+.1f", mv, in.depth, in.score / 100.0);
    }
    (void)u;
    s_thinking = false;
}
