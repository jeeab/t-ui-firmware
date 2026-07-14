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

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// --- bridges into the device-ui side (drawing) --------------------------------
extern "C" void tdeck_ui_label(int id, int x, int y, const char *text, uint32_t color);
extern "C" void tdeck_ui_box(int id, int x, int y, int w, int h, uint32_t color);
extern "C" void tdeck_ui_hide(int id);
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
static int api_screen_label(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    const char *s = luaL_checkstring(L, 4);
    uint32_t color = (uint32_t)luaL_optinteger(L, 5, 0xFFFFFF);
    tdeck_ui_label(id, x, y, s, color);
    return 0;
}

static int api_screen_box(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    uint32_t color = (uint32_t)luaL_optinteger(L, 6, 0xFFFFFF);
    tdeck_ui_box(id, x, y, w, h, color);
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

    static const luaL_Reg screenLib[] = {
        {"label", api_screen_label}, {"box", api_screen_box}, {"hide", api_screen_hide}, {nullptr, nullptr}};
    static const luaL_Reg deviceLib[] = {{"beep", api_device_beep}, {"time", api_device_time}, {nullptr, nullptr}};
    luaL_newlib(AppL, screenLib);
    lua_setglobal(AppL, "screen");
    luaL_newlib(AppL, deviceLib);
    lua_setglobal(AppL, "device");

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
