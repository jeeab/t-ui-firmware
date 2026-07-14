// -----------------------------------------------------------------------------
// LuaApp — device-ui side of the Lua app engine (firmware side: src/TDeckLua.cpp).
//
// Stage 3: apps run from Lua scripts ON THE SD CARD. The device seeds the card
// with the bundled apps (their .lua files under /apps/<name>/) and then loads +
// runs them straight off the card — so they're real files you can find, copy, and
// eventually replace with your own. Drawing is retained-by-id (no per-frame
// churn); a fresh screen is built per open and torn down on exit; the Lua heap
// lives in PSRAM. Toolbox: screen.label / screen.box / screen.hide / device.beep
// / device.time, plus on_open / on_tick / on_touch / on_close.
// -----------------------------------------------------------------------------
#include "graphics/common/SdCard.h" // SDFs (shared SdFat instance)
#include "lvgl.h"
#include <cstring>

extern "C" void lua_app_open(void);   // "Lua" tile — the hello demo
extern "C" void breakout_open(void);  // "Breakout" tile — the game
extern "C" void metronome_open(void); // "Metronome" tile — a tool app

// firmware-side engine (linked across the boundary)
extern "C" int tdeck_lua_app_start(const char *script);
extern "C" void tdeck_lua_app_tick(uint32_t dt);
extern "C" void tdeck_lua_app_touch(int x, int y);
extern "C" void tdeck_lua_app_drag(int x, int y);
extern "C" void tdeck_lua_app_stop(void);

namespace
{
lv_obj_t *luaScreen = nullptr;
lv_timer_t *luaTick = nullptr;

struct UObj {
    int id;
    int type; // 0 = label, 1 = box
    lv_obj_t *obj;
};
UObj uobjs[64];
int uobjCount = 0;
char scriptBuf[6144]; // a loaded main.lua

lv_obj_t *findOrCreate(int id, int type)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id && uobjs[i].type == type)
            return uobjs[i].obj;
    if (uobjCount >= 64 || !luaScreen)
        return nullptr;
    lv_obj_t *o;
    if (type == 0) {
        o = lv_label_create(luaScreen);
    } else {
        o = lv_obj_create(luaScreen);
        lv_obj_remove_style_all(o);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); // taps fall through to the screen
    uobjs[uobjCount].id = id;
    uobjs[uobjCount].type = type;
    uobjs[uobjCount].obj = o;
    uobjCount++;
    return o;
}

void tickCb(lv_timer_t *) { tdeck_lua_app_tick(33); }

// Write the bundled script to the SD card (overwrite so a firmware update always
// wins for OUR demos; user-added apps live in other folders and are never touched).
void seedApp(const char *dir, const char *path, const char *content)
{
    SDFs.mkdir("/apps");
    SDFs.mkdir(dir);
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) {
        f.write((const uint8_t *)content, strlen(content));
        f.close();
    }
}

// Load a script off the SD card into scriptBuf; returns nullptr if it can't.
const char *loadScript(const char *path)
{
    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f)
        return nullptr;
    int n = f.read((uint8_t *)scriptBuf, sizeof(scriptBuf) - 1);
    f.close();
    if (n <= 0)
        return nullptr;
    scriptBuf[n] = 0;
    return scriptBuf;
}

// Common open path: seed the card, load the script FROM the card, run it.
void runFromSd(const char *dir, const char *path, const char *bundled)
{
    if (luaScreen) {
        lv_obj_delete(luaScreen);
        luaScreen = nullptr;
    }
    uobjCount = 0;

    luaScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(luaScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(luaScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(luaScreen, LV_OBJ_FLAG_CLICKABLE);

    // NOTE: we no longer seed here — install happens once at boot (lua_seed_bundled),
    // so opening an app never re-creates a file the user deleted (that would defeat
    // uninstall). We just load whatever's on the card, falling back to the built-in
    // copy if the card is unavailable.
    (void)dir;
    const char *script = loadScript(path); // genuinely runs off the SD card
    if (!script)
        script = bundled; // SD unavailable / not installed -> fall back so it still works
    if (!script)
        return; // nothing to run (a user app whose file is gone) — bail safely
    tdeck_lua_app_start(script);

    if (!luaTick)
        luaTick = lv_timer_create(tickCb, 33, NULL);
    else
        lv_timer_resume(luaTick);

    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            lv_indev_t *d = lv_indev_active();
            if (!d)
                return;
            lv_point_t p;
            lv_indev_get_point(d, &p);
            tdeck_lua_app_touch((int)p.x, (int)p.y);
        },
        LV_EVENT_CLICKED, NULL);
    // Continuous position while the finger is down + moving -> on_drag (paddle steering, etc.)
    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            lv_indev_t *d = lv_indev_active();
            if (!d)
                return;
            lv_point_t p;
            lv_indev_get_point(d, &p);
            tdeck_lua_app_drag((int)p.x, (int)p.y);
        },
        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(
        luaScreen,
        [](lv_event_t *) {
            if (luaTick)
                lv_timer_pause(luaTick);
            tdeck_lua_app_stop();
        },
        LV_EVENT_SCREEN_UNLOADED, NULL);

    lv_screen_load_anim(luaScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ---- bundled app scripts (seeded to the SD card, then loaded from it) ---------
const char *HELLO_SCRIPT = R"LUA(
local x,y = 150,110
local dx,dy = 4,3
function on_open()
  screen.label(1, 8, 8, 'Hello from Lua!', 0xffffff)
  screen.label(2, 8, 220, 'this app is a file on your SD card', 0x8e8e93)
end
function on_tick(dt)
  x = x + dx; y = y + dy
  if x < 8 or x > 290 then dx = -dx end
  if y < 40 or y > 208 then dy = -dy end
  screen.box(3, x, y, 22, 22, 0x30d158)
end
function on_touch(tx, ty) device.beep() end
)LUA";

const char *BREAKOUT_SCRIPT = R"LUA(
local W = 320
local pw, ph = 56, 10
local py = 204        -- paddle sits a little higher now, to leave room for a
local sy = 224        -- steer strip UNDERNEATH it (drag the strip to move the paddle)
local px, pt = 132, 132
local bx, by = 156, 150
local vx, vy = 3, -3
local bs = 8
local sc, st = 0, 0
local cols, rows = 8, 4
local bw, bh, gp, lx, ty = 36, 14, 3, 6, 30
local col = {0xff453a, 0xff9f0a, 0xffd60a, 0x30d158}
local live = {}

local function bricks()
  for r = 0, rows-1 do
    for c = 0, cols-1 do
      live[r*cols+c] = true
      screen.box(100 + r*cols+c, lx + c*(bw+gp), ty + r*(bh+gp), bw, bh, col[r+1])
    end
  end
end

local function steer(x)
  pt = x - pw//2
  if pt < 0 then pt = 0 end
  if pt > W - pw then pt = W - pw end
end

local function reset()
  sc, st = 0, 0
  px, pt = 132, 132
  bx, by, vx, vy = 156, 150, 3, -3
  screen.label(3, 8, 8, 'Score 0', 0xffffff)
  screen.label(4, 8, 112, '', 0x000000)
  bricks()
end

function on_open()
  screen.label(5, 150, 8, 'drag the bar to steer', 0x6e6e73)
  screen.box(20, 10, sy, 300, 12, 0x2c2c2e) -- the steer strip
  reset()
end

function on_touch(x, y)
  if st ~= 0 then reset() return end
  steer(x)
end

function on_drag(x, y)
  if st == 0 then steer(x) end
end

function on_tick(dt)
  if st == 0 then
    local d = pt - px
    if d > 14 then d = 14 elseif d < -14 then d = -14 end
    px = px + d
    bx = bx + vx
    by = by + vy
    if bx < 0 then bx = 0; vx = -vx end
    if bx > W - bs then bx = W - bs; vx = -vx end
    if by < 24 then by = 24; vy = -vy end
    if by + bs >= py and by <= py + ph and bx + bs >= px and bx <= px + pw then
      vy = -math.abs(vy)
      local h = (bx + bs//2) - (px + pw//2)
      vx = h // 8
      if vx == 0 then vx = 1 end
      device.beep()
    end
    local done = false
    for r = 0, rows-1 do
      if done then break end
      for c = 0, cols-1 do
        local i = r*cols + c
        if live[i] then
          local qx = lx + c*(bw+gp)
          local qy = ty + r*(bh+gp)
          if bx + bs >= qx and bx <= qx + bw and by + bs >= qy and by <= qy + bh then
            live[i] = false
            screen.hide(100 + i)
            vy = -vy
            sc = sc + 10
            screen.label(3, 8, 8, 'Score ' .. sc, 0xffffff)
            device.beep()
            done = true
            break
          end
        end
      end
    end
    if by > 240 then
      st = 1
      screen.label(4, 60, 112, 'Game Over - tap to retry', 0xff453a)
    end
    local any = false
    for i = 0, rows*cols - 1 do if live[i] then any = true break end end
    if not any then
      st = 2
      screen.label(4, 92, 112, 'YOU WIN! - tap', 0x30d158)
    end
  end
  screen.box(2, px, py, pw, ph, 0x0a84ff)                -- paddle
  screen.box(1, bx, by, bs, bs, 0xffffff)                -- ball
  screen.box(21, px + pw//2 - 8, sy + 1, 16, 10, 0x0a84ff) -- knob on the steer strip, tracks paddle
end
)LUA";

// A simple TOOL app: a metronome. Swinging bob, a loud tick each beat, tap the
// bottom-left / bottom-right to slow down / speed up. Runs off the SD card.
const char *METRONOME_SCRIPT = R"LUA(
local bpm = 100
local acc = 0
local dir = 1
local minx, maxx = 36, 268   -- bob travel (bob is 16 wide)
local function interval() return 60000 // bpm end
local function show()
  screen.label(1, 116, 14, bpm .. ' BPM', 0xffffff)
end
function on_open()
  screen.box(5, 158, 60, 4, 120, 0x3a3a3c)   -- center rod
  screen.label(2, 30, 208, '-  slower', 0xff9f0a)
  screen.label(3, 214, 208, 'faster  +', 0x30d158)
  show()
end
function on_tick(dt)
  acc = acc + dt
  local iv = interval()
  if acc >= iv then
    acc = acc - iv
    dir = -dir
    device.beep(true)   -- loud tick
  end
  local frac = acc / iv
  local p = frac
  if dir < 0 then p = 1 - frac end
  local x = minx + math.floor((maxx - minx) * p)
  screen.box(10, x, 150, 16, 40, 0x0a84ff)   -- the bob
end
function on_touch(tx, ty)
  if ty > 190 then
    if tx < 160 then bpm = bpm - 4 else bpm = bpm + 4 end
    if bpm < 30 then bpm = 30 end
    if bpm > 240 then bpm = 240 end
    show()
  end
end
)LUA";
} // namespace

// ---- drawing bridges the Lua toolbox calls (from src/TDeckLua.cpp) -----------
extern "C" void tdeck_ui_label(int id, int x, int y, const char *text, uint32_t color)
{
    lv_obj_t *o = findOrCreate(id, 0);
    if (!o)
        return;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(o, text ? text : "");
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
}

extern "C" void tdeck_ui_box(int id, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = findOrCreate(id, 1);
    if (!o)
        return;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 4, 0);
}

extern "C" void tdeck_ui_hide(int id)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id) {
            lv_obj_add_flag(uobjs[i].obj, LV_OBJ_FLAG_HIDDEN);
            return;
        }
}

// ---- launcher entry points ---------------------------------------------------
extern "C" void lua_app_open(void) { runFromSd("/apps/hello", "/apps/hello/main.lua", HELLO_SCRIPT); }
extern "C" void breakout_open(void) { runFromSd("/apps/breakout", "/apps/breakout/main.lua", BREAKOUT_SCRIPT); }
extern "C" void metronome_open(void) { runFromSd("/apps/metronome", "/apps/metronome/main.lua", METRONOME_SCRIPT); }

// Launch ANY user app by its SD-card script path (no bundled fallback). Used by the
// launcher for apps a user dropped onto the card themselves.
extern "C" void lua_run_path(const char *path)
{
    if (path && SDFs.exists(path))
        runFromSd("", path, nullptr);
}

// First-run installer: seed the bundled SD apps ONCE per firmware version. After
// this, the apps are files the user owns — delete a folder to uninstall it and it
// stays gone (we never re-seed unless the version marker bumps). Bump the marker
// name when a bundled script changes so the update reinstalls.
extern "C" void lua_seed_bundled(void)
{
    // Install each bundled app ONCE — only when its folder doesn't exist yet. The folder
    // then acts as a permanent tombstone: deleting an app's main.lua (a FILE, which the
    // on-device Files app can delete) uninstalls it for good, because the leftover folder
    // stops it from ever being recreated. (Removing the whole folder = a fresh reinstall.)
    SDFs.mkdir("/apps");
    if (!SDFs.exists("/apps/breakout"))
        seedApp("/apps/breakout", "/apps/breakout/main.lua", BREAKOUT_SCRIPT);
    if (!SDFs.exists("/apps/metronome"))
        seedApp("/apps/metronome", "/apps/metronome/main.lua", METRONOME_SCRIPT);
    if (!SDFs.exists("/apps/hello"))
        seedApp("/apps/hello", "/apps/hello/main.lua", HELLO_SCRIPT);
}
