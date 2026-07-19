// -----------------------------------------------------------------------------
// TDeckLua — the T-Deck Lua app engine (firmware side).
//
// Split so neither side has to include the other's headers:
//   • Firmware side (here): owns the lua_State, opens ONLY safe libraries, and
//     registers the toolbox (screen.*/device.*). The toolbox C functions pull
//     args off the Lua stack and forward to device-ui bridges (tdeck_ui_*),
//     which do the actual LVGL drawing. So this file needs lua.h, not lvgl.h.
//   • Device-ui side (LuaApp.cpp): owns the screen + objects + tick/touch, and
//     calls tdeck_lua_app_* here. So it needs lvgl.h, not lua.h.
//
// A buggy app can only reach the toolbox (the fence) and every hook runs inside
// lua_pcall, so a script error can't take the firmware down.
// -----------------------------------------------------------------------------
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// --- bridges into the device-ui side (drawing + the app-folder file store) -----
extern "C" void tdeck_ui_label(int id, int x, int y, const char *text, uint32_t color);
extern "C" void tdeck_ui_box(int id, int x, int y, int w, int h, uint32_t color, int radius);
extern "C" void tdeck_ui_line(int id, int x1, int y1, int x2, int y2, int thickness, uint32_t color);
extern "C" void tdeck_ui_hide(int id);
extern "C" int tdeck_appfs_read(const char *name, char *buf, int cap);
extern "C" bool tdeck_appfs_write(const char *name, const char *data, int len);
// Multi-touch: read up to `max` fingers currently on the screen (the GT911 tracks
// several; LVGL only ever consumes one). Returns the count, or -1 if no reader is
// wired up (pre-16.4 device-ui). Lets games use both halves of the screen at once.
extern "C" int tdeck_touch_read(unsigned short *xs, unsigned short *ys, int max);
// --- other firmware helpers ---------------------------------------------------
void playBeep();                              // buzz.cpp (C++ linkage)
extern "C" void tdeck_beep_gain(float gain);  // TDeckBeep.cpp — temporarily boost buzzer volume

extern "C" void tdeck_lua_app_stop(void); // defined below; used by tdeck_lua_app_start

static lua_State *AppL = nullptr;

// Route ALL Lua allocations to PSRAM (the big 8MB pool) so apps never starve the
// scarce internal RAM the mesh stack lives in — this is how the engine scales.
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        heap_caps_free(ptr);
        return nullptr;
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

// ---- the toolbox: the ONLY things a Lua app can reach ------------------------
// Coordinates arrive from game maths, so they are routinely fractional (136.3, not 136).
// luaL_checkinteger THROWS on those in Lua 5.4 ("number has no integer representation"),
// which killed Starfield's whole draw loop the moment its ship drifted off a whole pixel -
// the app just stopped updating with no visible error. Take any number and floor it.
static inline int pixArg(lua_State *L, int i)
{
    return (int)floor(luaL_checknumber(L, i));
}
static inline uint32_t colArg(lua_State *L, int i, uint32_t dflt)
{
    return (uint32_t)luaL_optnumber(L, i, (lua_Number)dflt);
}

static int api_screen_label(lua_State *L)
{
    int id = pixArg(L, 1);
    int x = pixArg(L, 2);
    int y = pixArg(L, 3);
    const char *s = luaL_checkstring(L, 4);
    uint32_t color = colArg(L, 5, 0xFFFFFF);
    tdeck_ui_label(id, x, y, s, color);
    return 0;
}

static int api_screen_box(lua_State *L)
{
    int id = pixArg(L, 1);
    int x = pixArg(L, 2);
    int y = pixArg(L, 3);
    int w = pixArg(L, 4);
    int h = pixArg(L, 5);
    uint32_t color = colArg(L, 6, 0xFFFFFF);
    int radius = (int)luaL_optnumber(L, 7, 4); // pass w (or more) with w==h for a circle
    tdeck_ui_box(id, x, y, w, h, color, radius);
    return 0;
}

// store.read(name) -> file contents as a string, or nil if it doesn't exist.
// Jailed to the app's own /apps/<name>/ folder (enforced device-ui side).
static int api_store_read(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    static char buf[4096];
    int n = tdeck_appfs_read(name, buf, sizeof(buf));
    if (n < 0)
        lua_pushnil(L);
    else
        lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

// store.write(name, content) -> true/false. Content capped at 4KB.
static int api_store_write(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > 4096) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, tdeck_appfs_write(name, data, (int)len));
    return 1;
}

// screen.line(id, x1, y1, x2, y2, thickness, color) — an angled thick bar.
// ---- canvas: pixel drawing for games ----------------------------------------
// The screen.* API above is capped at 80 UI objects. The canvas is one object holding
// a whole frame buffer, so an app can draw as much as it likes. Every call here lands
// in C, which is what keeps it fast enough to be worth having.
extern "C" int tdeck_ui_canvas_begin(void);
extern "C" void tdeck_ui_canvas_clear(uint32_t color);
extern "C" void tdeck_ui_canvas_rect(int x, int y, int w, int h, uint32_t color);
extern "C" void tdeck_ui_canvas_pixel(int x, int y, uint32_t color);
extern "C" void tdeck_ui_canvas_line(int x1, int y1, int x2, int y2, uint32_t color);
extern "C" void tdeck_ui_canvas_circle(int cx, int cy, int r, uint32_t color, int filled);
extern "C" void tdeck_ui_canvas_flip(void);

static int api_canvas_begin(lua_State *L)
{
    lua_pushboolean(L, tdeck_ui_canvas_begin());
    return 1;
}

static int api_canvas_clear(lua_State *L)
{
    tdeck_ui_canvas_clear((uint32_t)luaL_optinteger(L, 1, 0x000000));
    return 0;
}

static int api_canvas_rect(lua_State *L)
{
    tdeck_ui_canvas_rect(pixArg(L, 1), pixArg(L, 2), pixArg(L, 3),
                         pixArg(L, 4), colArg(L, 5, 0xFFFFFF));
    return 0;
}

static int api_canvas_pixel(lua_State *L)
{
    tdeck_ui_canvas_pixel(pixArg(L, 1), pixArg(L, 2),
                          colArg(L, 3, 0xFFFFFF));
    return 0;
}

static int api_canvas_line(lua_State *L)
{
    tdeck_ui_canvas_line(pixArg(L, 1), pixArg(L, 2), pixArg(L, 3),
                         pixArg(L, 4), colArg(L, 5, 0xFFFFFF));
    return 0;
}

static int api_canvas_circle(lua_State *L)
{
    tdeck_ui_canvas_circle(pixArg(L, 1), pixArg(L, 2), pixArg(L, 3),
                           colArg(L, 4, 0xFFFFFF), lua_toboolean(L, 5));
    return 0;
}

extern "C" void tdeck_ui_canvas_sprite(int x, int y, int w, int h, const char *pix, const uint32_t *lut,
                                       const uint8_t *opaque, int scale);

// canvas.sprite(x, y, width, pixels, palette, scale)
//
//   pixels  - one character per pixel, read left to right, top to bottom
//   palette - table mapping a character to 0xRRGGBB. Characters with no entry (space,
//             by convention) are transparent.
//   scale   - optional, draws each pixel as a block this many screen pixels across
//
// The palette is flattened into a 95-entry lookup here, ONCE per call, so the blit loop
// in C never has to reach back into Lua. That keeps a 16x16 sprite at 256 array reads
// instead of 256 Lua table lookups.
static int api_canvas_sprite(lua_State *L)
{
    int x = pixArg(L, 1);
    int y = pixArg(L, 2);
    int w = pixArg(L, 3);
    size_t len = 0;
    const char *pix = luaL_checklstring(L, 4, &len);
    luaL_checktype(L, 5, LUA_TTABLE);
    int scale = (int)luaL_optnumber(L, 6, 1);

    if (w <= 0 || len == 0)
        return 0;
    int h = (int)(len / (size_t)w);
    if (h <= 0)
        return 0;

    uint32_t lut[95];
    uint8_t opaque[95];
    memset(lut, 0, sizeof(lut));
    memset(opaque, 0, sizeof(opaque));
    for (int c = 32; c <= 126; c++) {
        char key[2] = {(char)c, 0};
        lua_pushstring(L, key);
        lua_gettable(L, 5);
        if (lua_isnumber(L, -1)) {
            lut[c - 32] = (uint32_t)lua_tointeger(L, -1);
            opaque[c - 32] = 1;
        }
        lua_pop(L, 1);
    }

    tdeck_ui_canvas_sprite(x, y, w, h, pix, lut, opaque, scale);
    return 0;
}

static int api_canvas_flip(lua_State *L)
{
    (void)L;
    tdeck_ui_canvas_flip();
    return 0;
}

static int api_screen_line(lua_State *L)
{
    int id = pixArg(L, 1);
    int x1 = pixArg(L, 2);
    int y1 = pixArg(L, 3);
    int x2 = pixArg(L, 4);
    int y2 = pixArg(L, 5);
    int thickness = (int)luaL_optnumber(L, 6, 4);
    uint32_t color = colArg(L, 7, 0xFFFFFF);
    tdeck_ui_line(id, x1, y1, x2, y2, thickness, color);
    return 0;
}

static int api_screen_hide(lua_State *L)
{
    tdeck_ui_hide((int)luaL_checkinteger(L, 1));
    return 0;
}

static int api_device_beep(lua_State *L)
{
    // device.beep()      -> normal (gentle) beep
    // device.beep(true)  -> loud beep (e.g. a metronome tick). playBeep busy-waits,
    //                       so bracketing the gain around it is safe.
    if (lua_toboolean(L, 1)) {
        tdeck_beep_gain(1.0f);
        playBeep();
        tdeck_beep_gain(0.2f);
    } else {
        playBeep();
    }
    return 0;
}

static int api_device_time(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

// device.touches() -> n, x1, y1, x2, y2 — every finger on the screen right now (up
// to 2). n is 0 when nothing is touching. Poll it from on_tick for true multi-touch
// (e.g. pinball holding both flippers); on_touch/on_drag stay single-point.
static int api_device_touches(lua_State *L)
{
    unsigned short xs[2], ys[2];
    int n = tdeck_touch_read(xs, ys, 2);
    if (n < 0)
        n = 0;
    lua_pushinteger(L, n);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)xs[i]);
        lua_pushinteger(L, (lua_Integer)ys[i]);
    }
    return 1 + 2 * n;
}

static void call_optional(const char *fn)
{
    if (!AppL)
        return;
    lua_getglobal(AppL, fn);
    if (lua_isfunction(AppL, -1)) {
        if (lua_pcall(AppL, 0, 0, 0) != LUA_OK)
            lua_pop(AppL, 1); // discard the error message
    } else {
        lua_pop(AppL, 1);
    }
}

// ---- lifecycle, driven by the device-ui side ---------------------------------

// Load + run an app script (defines its on_* functions), then call on_open().
// Returns 0 on success, negative on failure.
extern "C" int tdeck_lua_app_start(const char *script)
{
    tdeck_lua_app_stop(); // tear down any previous app first

    AppL = lua_newstate(lua_psram_alloc, nullptr, 0); // apps live in PSRAM (3rd arg = hash seed)
    if (!AppL)
        return -1;

    // open ONLY the safe libraries — no io/os/package. That omission is the fence.
    luaL_requiref(AppL, LUA_GNAME, luaopen_base, 1);
    lua_pop(AppL, 1);
    luaL_requiref(AppL, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(AppL, 1);
    luaL_requiref(AppL, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(AppL, 1);
    luaL_requiref(AppL, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(AppL, 1);

    static const luaL_Reg screenLib[] = {{"label", api_screen_label},
                                         {"box", api_screen_box},
                                         {"line", api_screen_line},
                                         {"hide", api_screen_hide},
                                         {nullptr, nullptr}};
    static const luaL_Reg deviceLib[] = {
        {"beep", api_device_beep}, {"time", api_device_time}, {"touches", api_device_touches}, {nullptr, nullptr}};
    static const luaL_Reg storeLib[] = {{"read", api_store_read}, {"write", api_store_write}, {nullptr, nullptr}};
    static const luaL_Reg canvasLib[] = {{"begin", api_canvas_begin}, {"clear", api_canvas_clear},
                                         {"rect", api_canvas_rect},   {"pixel", api_canvas_pixel},
                                         {"line", api_canvas_line},   {"circle", api_canvas_circle},
                                         {"sprite", api_canvas_sprite},
                                         {"flip", api_canvas_flip},   {nullptr, nullptr}};
    luaL_newlib(AppL, screenLib);
    lua_setglobal(AppL, "screen");
    luaL_newlib(AppL, deviceLib);
    lua_setglobal(AppL, "device");
    luaL_newlib(AppL, storeLib);
    lua_setglobal(AppL, "store");
    luaL_newlib(AppL, canvasLib);
    lua_setglobal(AppL, "canvas");

    if (luaL_dostring(AppL, script) != LUA_OK) {
        lua_close(AppL);
        AppL = nullptr;
        return -2;
    }

    call_optional("on_open");
    return 0;
}

extern "C" void tdeck_lua_app_tick(uint32_t dt)
{
    if (!AppL)
        return;
    lua_getglobal(AppL, "on_tick");
    if (!lua_isfunction(AppL, -1)) {
        lua_pop(AppL, 1);
        return;
    }
    lua_pushinteger(AppL, (lua_Integer)dt);
    if (lua_pcall(AppL, 1, 0, 0) != LUA_OK)
        lua_pop(AppL, 1);
}

extern "C" void tdeck_lua_app_touch(int x, int y)
{
    if (!AppL)
        return;
    lua_getglobal(AppL, "on_touch");
    if (!lua_isfunction(AppL, -1)) {
        lua_pop(AppL, 1);
        return;
    }
    lua_pushinteger(AppL, x);
    lua_pushinteger(AppL, y);
    if (lua_pcall(AppL, 2, 0, 0) != LUA_OK)
        lua_pop(AppL, 1);
}

// Continuous finger position while dragging (fires many times per press). Apps
// opt in by defining on_drag(x, y) — used e.g. by Breakout to steer the paddle.
extern "C" void tdeck_lua_app_drag(int x, int y)
{
    if (!AppL)
        return;
    lua_getglobal(AppL, "on_drag");
    if (!lua_isfunction(AppL, -1)) {
        lua_pop(AppL, 1);
        return;
    }
    lua_pushinteger(AppL, x);
    lua_pushinteger(AppL, y);
    if (lua_pcall(AppL, 2, 0, 0) != LUA_OK)
        lua_pop(AppL, 1);
}

// A key from the physical keyboard. Apps opt in by defining on_key(k).
//
// Printable characters arrive as themselves ("a", "7", " "). The keys that aren't
// characters arrive as names — "left", "right", "up", "down", "enter", "back", "esc" —
// so an app can read `if k == "left"` instead of memorising control codes.
extern "C" void tdeck_lua_app_key(uint32_t key)
{
    if (!AppL)
        return;
    lua_getglobal(AppL, "on_key");
    if (!lua_isfunction(AppL, -1)) {
        lua_pop(AppL, 1);
        return;
    }

    char one[2] = {0, 0};
    const char *name = nullptr;
    switch (key) {
    case 17: name = "up"; break;
    case 18: name = "down"; break;
    case 19: name = "right"; break;
    case 20: name = "left"; break;
    case 27: name = "esc"; break;
    case 8:  name = "back"; break;  // backspace
    case 127: name = "back"; break; // delete — same intent to a player
    case 10:
    case 13: name = "enter"; break;
    default:
        if (key >= 32 && key < 127) {
            one[0] = (char)key;
            name = one;
        }
        break;
    }
    if (!name) { // something we don't have a name for: don't invent one
        lua_pop(AppL, 1);
        return;
    }

    lua_pushstring(AppL, name);
    if (lua_pcall(AppL, 1, 0, 0) != LUA_OK)
        lua_pop(AppL, 1);
}

extern "C" void tdeck_lua_app_stop(void)
{
    if (!AppL)
        return;
    call_optional("on_close");
    lua_close(AppL);
    AppL = nullptr;
}

// ---- Stage-1 self-test (launcher shows "Lua:5050" if Lua runs on-device) ------
extern "C" int tdeck_lua_selftest(void)
{
    lua_State *L = luaL_newstate();
    if (!L)
        return -1;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    int result = -1;
    if (luaL_dostring(L, "local s=0; for i=1,100 do s=s+i end; return s") == LUA_OK) {
        if (lua_isinteger(L, -1))
            result = (int)lua_tointeger(L, -1);
        else if (lua_isnumber(L, -1))
            result = (int)lua_tonumber(L, -1);
    }
    lua_close(L);
    return result;
}
