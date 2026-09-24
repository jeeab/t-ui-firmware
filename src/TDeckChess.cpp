#include "configuration.h"

#include "TDeckChess.h"
#include "chess/chess.h"
#include "chess/search.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include "graphics/common/SdCard.h"

// -----------------------------------------------------------------------------------------
// The chess game. Jake, 2026-09-22: "local chess with a chess ai kinda thing with difficulty
// settings up to master".
//
// ⛔ THE SEARCH RUNS ON ITS OWN TASK, not the UI task and not the main loop. Measured on this
// chip the engine does ~16-20 thousand positions a second, so a deep think is many seconds. The
// UI task holds spiLock while it runs, so searching there would freeze the screen and stall the
// radio together; the main loop is where Meshtastic services the radio, so searching THERE stops
// packets being drained on a device that hears 248 nodes. A separate low-priority task blocks
// neither. Jake: "I don't mind if it takes 10 seconds of chess doesn't freeze anything."
//
// ⭐ AND IT SEARCHES A COPY OF THE BOARD. Alpha-beta makes and unmakes moves as it goes, so the
// position is transiently wrong for the whole search - and the UI reads the real one to draw.
// -----------------------------------------------------------------------------------------

static Board s_board;
static bool s_ready = false;
static volatile bool s_thinking = false;

// ⭐ THE ENGINE SEARCHES A COPY, NEVER THE LIVE BOARD. Alpha-beta makes and unmakes moves as it
// goes, so the position it is working on is transiently wrong throughout - and the UI reads the
// real one to draw the screen. Only the move it settles on is applied.
static Board s_searchBoard;
static Move s_result;
static volatile bool s_applyPending = false;

// Its own task, so ten seconds of thinking blocks nothing: not the screen, not the radio, not
// the main loop. Priority 1 - the same as the UI task and below the loop - so it yields to both.
static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_go = nullptr;
static const uint32_t kThinkMs = 10000; // Jake: "I don't mind if it takes 10 seconds"
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

// ⭐ ONE LINE OF FEN. The standard way to write a position down, and chess_get_fen/set_fen
// already exist for the perft tests. Written after every move - a game is a handful of moves an
// hour, so the SD cost is nothing, and the alternative is losing the game to a flat battery.
static const char *kSaveFile = "/chess.fen";

static void saveGame(void)
{
    char fen[100];
    chess_get_fen(&s_board, fen, sizeof(fen));
    FsFile f = SDFs.open(kSaveFile, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return; // no card, or the bus is busy: the game simply is not saved this move
    f.println(fen);
    f.close();
}

static bool loadGame(void)
{
    FsFile f = SDFs.open(kSaveFile, O_RDONLY);
    if (!f)
        return false;
    char fen[100] = {0};
    const int n = f.fgets(fen, sizeof(fen));
    f.close();
    if (n < 10)
        return false;
    for (char *p = fen; *p; p++)
        if (*p == '\n' || *p == '\r') {
            *p = 0;
            break;
        }
    // ⛔ A REJECTED FEN MUST NOT LEAVE A HALF-PARSED BOARD. chess_set_fen writes as it goes, so
    // parse into a scratch board and only adopt it if the whole thing was valid.
    Board tmp;
    if (!chess_set_fen(&tmp, fen))
        return false;
    s_board = tmp;
    return true;
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
    if (loadGame())
        strncpy(s_status, s_board.side == WHITE ? "Your move" : "My move", sizeof(s_status) - 1);
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
    s_applyPending = false;
    saveGame();
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
    saveGame();
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

static void chessTask(void *)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        Move m;
        const bool ok = search_best_move(&s_searchBoard, (ChessLevel)s_level, kThinkMs, &m);
        s_result = m;
        s_applyPending = ok;
        s_thinking = false; // the service on the main loop picks it up from here
        if (!ok)
            strncpy(s_status, "No move", sizeof(s_status) - 1);
    }
}

bool tdeck_chess_request_engine(void)
{
    ensureInit();
    if (s_thinking || chess_game_over(&s_board))
        return false;
    if (!s_go) {
        s_go = xSemaphoreCreateBinary();
        if (!s_go)
            return false;
    }
    if (!s_task) {
        // 10KB. The move lists live in PSRAM now (see search.c), so the search itself uses very
        // little stack - but "very little" is worth measuring rather than assuming, and
        // tdeck_chess_stack_free() reports the high-water mark.
        if (xTaskCreate(chessTask, "chess", 10240, nullptr, 1, &s_task) != pdPASS) {
            s_task = nullptr;
            return false;
        }
    }
    s_searchBoard = s_board; // the copy it is allowed to scribble on
    s_applyPending = false;
    s_thinking = true;
    strncpy(s_status, "Thinking...", sizeof(s_status) - 1);
    xSemaphoreGive(s_go);
    return true;
}

// Free the thinking task when the app is put away. Only safe when it is idle.
extern "C" void tdeck_chess_release(void)
{
    if (s_thinking || !s_task)
        return;
    vTaskDelete(s_task);
    s_task = nullptr;
}

extern "C" unsigned tdeck_chess_stack_free(void)
{
    return s_task ? (unsigned)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)) : 0;
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
    saveGame();
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
    // The thinking happens on the chess task. All this does is apply the answer, on the main
    // loop, so the move lands on the real board from one place only.
    if (!s_applyPending || s_thinking)
        return;
    s_applyPending = false;
    if (!s_ready)
        return;
    const Move m = s_result;
    Undo u;
    if (s_undoN < UNDO_MAX) {
        chess_make(&s_board, &m, &s_undo[s_undoN++]);
        search_history_push(s_board.hash);
        s_lastFrom = m.from;
        s_lastTo = m.to;
    }

    saveGame();
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
