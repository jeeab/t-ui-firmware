// -----------------------------------------------------------------------------
// LuaApp — device-ui side of the Lua app engine (firmware side: src/TDeckLua.cpp).
//
// Stage 3: apps run from Lua scripts ON THE SD CARD. The device seeds the card
// with the bundled apps (their .lua files under /apps/<name>/) and then loads +
// runs them straight off the card — so they're real files you can find, copy, and
// eventually replace with your own. Drawing is retained-by-id (no per-frame
// churn); a fresh screen is built per open and torn down on exit; the Lua heap
// lives in PSRAM. Toolbox: screen.label / screen.box / screen.hide / device.beep
// / device.time / store.read / store.write, plus on_open / on_tick / on_touch /
// on_drag / on_close. store.* is jailed to the app's own /apps/<name>/ folder
// (simple filenames only) so apps can keep high scores etc. but can't touch
// anything else on the card.
// -----------------------------------------------------------------------------
#include "graphics/common/SdCard.h" // SDFs (shared SdFat instance)
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

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
    int type; // 0 = label, 1 = box, 2 = line
    lv_obj_t *obj;
    lv_point_precise_t pts[2]; // line endpoints (lv_line keeps a pointer to these — must persist)
    int32_t cp[5];             // last-drawn geometry — lets a repeat call skip LVGL entirely
    uint32_t ccol;             // last-drawn color
};
UObj uobjs[80];
constexpr int kMaxObj = 80;
int uobjCount = 0;
// Script buffer lives in PSRAM (16K — room for real games) so it doesn't eat the
// scarce internal heap. Allocated once on first use, kept for the session.
constexpr int kScriptCap = 16384;
char *scriptBuf = nullptr;
// The running app's own folder on the SD card ("" = none/SD unavailable). Set per
// launch; store.read/write are jailed inside it.
char appDir[64] = "";

// Returns the slot for (id,type), creating the LVGL object on first use. nullptr if full.
UObj *findOrCreateSlot(int id, int type)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id && uobjs[i].type == type)
            return &uobjs[i];
    if (uobjCount >= kMaxObj || !luaScreen)
        return nullptr;
    lv_obj_t *o;
    if (type == 0) {
        o = lv_label_create(luaScreen);
    } else if (type == 2) {
        o = lv_line_create(luaScreen);
        // pos/size are set per draw to the line's own bounding box — a full-screen
        // object here made every flipper move invalidate the whole 320x240
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    } else {
        o = lv_obj_create(luaScreen);
        lv_obj_remove_style_all(o);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); // taps fall through to the screen
    uobjs[uobjCount].id = id;
    uobjs[uobjCount].type = type;
    uobjs[uobjCount].obj = o;
    uobjs[uobjCount].cp[0] = INT32_MIN; // never matches -> first draw always lands
    uobjs[uobjCount].ccol = 0;
    return &uobjs[uobjCount++];
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
    if (!scriptBuf)
        scriptBuf = (char *)heap_caps_malloc(kScriptCap, MALLOC_CAP_SPIRAM);
    if (!scriptBuf)
        return nullptr;
    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f)
        return nullptr;
    int n = f.read((uint8_t *)scriptBuf, kScriptCap - 1);
    f.close();
    if (n <= 0)
        return nullptr;
    scriptBuf[n] = 0;
    return scriptBuf;
}

// Remember the app's own folder (from ".../main.lua" -> "..."), for store.* jail.
void setAppDir(const char *scriptPath)
{
    appDir[0] = 0;
    if (!scriptPath)
        return;
    const char *slash = strrchr(scriptPath, '/');
    if (!slash || slash == scriptPath)
        return;
    size_t n = (size_t)(slash - scriptPath);
    if (n >= sizeof(appDir))
        return;
    memcpy(appDir, scriptPath, n);
    appDir[n] = 0;
}

// A store filename is a plain name like "hiscore.txt" — no paths, no dot-dot.
bool storeNameOk(const char *name)
{
    if (!name || !name[0] || strlen(name) > 32)
        return false;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\'))
        return false;
    return true;
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
    setAppDir(path); // the app's own folder = its store.* jail
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

    // PRESSED (finger lands), not CLICKED (finger lifts) — a flipper that fires on
    // release feels like lag; every app here wants the tap the moment it happens.
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
        LV_EVENT_PRESSED, NULL);
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
-- Breakout 2.0 for T-UI ---------------------------------------------------
-- 5 levels (then they loop, faster), falling power-ups, 3 lives, and a high
-- score saved to hiscore.txt in this app's folder on the SD card.
-- Power-ups: W = wide paddle (12s) · L = extra life · B = +50 points
---------------------------------------------------------------------------
local W = 320
local basePw, widePw, ph, py = 56, 88, 10, 188
local pw = basePw
local sy, sh = 204, 36               -- the steer strip: big, for big thumbs
local bs = 8                         -- ball size
local px, pt = 132, 132              -- paddle x + where the thumb wants it
local bx, by = 156.0, 150.0          -- ball position
local vx, vy = 4.0, -4.8             -- ball velocity
local sc, lives, lv, loop = 0, 3, 1, 0
local hs = 0
local st = 0                         -- 0 play · 1 game over · 2 level clear · 3 ball lost
local waitUntil, wideUntil = 0, 0
local cols, rows = 8, 4
local bw, bh, gp, lx, ty = 36, 12, 3, 6, 28
local col = {0xff453a, 0xff9f0a, 0xffd60a, 0x30d158, 0x0a84ff}
local live = {}
local dtype, dpx, dpy = 0, 0, 0.0    -- falling power-up (0 = none)

local LEVELS = {
  {'11111111','22222222','33333333','44444444'},
  {'1.1.1.1.','.2.2.2.2','3.3.3.3.','.4.4.4.4'},
  {'...11...','..2222..','.333333.','44444444'},
  {'11.22.11','33.44.33','11.22.11','55.55.55'},
  {'55555555','44444444','33333333','22222222','11111111'},
  {'55555555','5......5','5..11..5','5......5','55555555'},
  {'1.2.3.4.','1.2.3.4.','1.2.3.4.','1.2.3.4.','1.2.3.4.'},
  {'...55...','..5445..','.543345.','..5445..','...55...'},
  {'22..22..','..33..33','44..44..','..55..55'},
  {'5.5.5.5.','44444444','33333333','2.2.2.2.','11111111'},
}

local function speed() return 4.4 + (lv - 1) * 3 / 5 + loop end

local function hud()
  screen.label(3, 4, 4, 'Score ' .. sc, 0xffffff)
  screen.label(6, 244, 4, 'Best ' .. hs, 0xffd60a)
  screen.label(7, 118, 4, 'Lv ' .. lv .. '   x' .. lives, 0x8e8e93)
end

local function msg(t, c)
  screen.label(4, 88, 108, t, c or 0xffffff)
end

local function buildLevel()
  local pat = LEVELS[lv]
  rows = #pat
  for i = 0, 39 do live[i] = false; screen.hide(100 + i) end
  for r = 1, rows do
    local row = pat[r]
    for c = 1, cols do
      local ch = string.sub(row, c, c)
      if ch ~= '.' then
        local i = (r - 1) * cols + (c - 1)
        live[i] = true
        screen.box(100 + i, lx + (c - 1) * (bw + gp), ty + (r - 1) * (bh + gp), bw, bh, col[tonumber(ch)])
      end
    end
  end
end

local function resetBall()
  bx, by = px + pw / 2 - bs / 2, 150.0
  local s = speed()
  vy = -s
  if math.random(2) == 1 then vx = s * 3 / 5 else vx = -s * 3 / 5 end
end

local function newGame()
  sc, lives, lv, loop = 0, 3, 1, 0
  pw, wideUntil = basePw, 0
  dtype = 0; screen.hide(30); screen.hide(31)
  st = 0
  msg('', 0)
  buildLevel()
  resetBall()
  hud()
end

local function steer(x)
  pt = x - pw // 2
  if pt < 0 then pt = 0 end
  if pt > W - pw then pt = W - pw end
end

local function saveScore()
  if sc > hs then
    hs = sc
    store.write('hiscore.txt', tostring(hs))
    hud()
  end
end

local function dropPowerup(x, y)
  if dtype ~= 0 or math.random(4) ~= 1 then return end
  local r = math.random(20)  -- weighted: wide 40%, points 45%, extra life only 15%
  if r <= 8 then dtype = 1 elseif r <= 17 then dtype = 3 else dtype = 2 end
  dpx, dpy = x + bw // 2 - 8, y + 0.0
end

local function applyPowerup()
  if dtype == 1 then wideUntil = device.time() + 12000
  elseif dtype == 2 then lives = lives + 1
  else sc = sc + 50 end
  hud()
  device.beep(true)
  dtype = 0; screen.hide(30); screen.hide(31)
end

function on_open()
  math.randomseed(device.time())
  hs = math.floor(tonumber(store.read('hiscore.txt') or '') or 0)
  screen.box(20, 0, sy, W, sh, 0x2c2c2e)                -- the steer strip
  screen.label(5, 104, sy + 10, 'drag here to steer', 0x6e6e73)
  newGame()
end

function on_touch(x, y)
  if st == 1 then newGame() return end
  steer(x)
end

function on_drag(x, y)
  if st ~= 1 then steer(x) end
end

function on_tick(dt)
  local now = device.time()

  -- wide-paddle power-up wears off
  local wantPw = basePw
  if now < wideUntil then wantPw = widePw end
  if wantPw ~= pw then pw = wantPw; steer(px + pw // 2) end

  if st == 2 or st == 3 then                -- brief pause, then next level / next ball
    if now >= waitUntil then
      if st == 2 then
        lv = lv + 1
        if lv > #LEVELS then lv = 1; loop = loop + 1 end
        buildLevel()
        hud()
      end
      msg('', 0)
      resetBall()
      st = 0
    end
  elseif st == 0 then
    -- paddle eases toward the thumb
    local d = pt - px
    if d > 16 then d = 16 elseif d < -16 then d = -16 end
    px = px + d

    bx = bx + vx
    by = by + vy
    if bx < 0 then bx = 0; vx = -vx end
    if bx > W - bs then bx = W - bs; vx = -vx end
    if by < 20 then by = 20; vy = -vy end

    -- paddle bounce (only while heading down), with edge english
    if vy > 0 and by + bs >= py and by <= py + ph and bx + bs >= px and bx <= px + pw then
      vy = -math.abs(vy)
      vx = ((bx + bs / 2) - (px + pw / 2)) / 4.5
      if vx > 7 then vx = 7 elseif vx < -7 then vx = -7 end
      if vx < 2 and vx > -2 then
        if vx < 0 then vx = -2 else vx = 2 end
      end
      device.beep()
    end

    -- brick hits
    local done = false
    for r = 0, rows - 1 do
      if done then break end
      for c = 0, cols - 1 do
        local i = r * cols + c
        if live[i] then
          local qx = lx + c * (bw + gp)
          local qy = ty + r * (bh + gp)
          if bx + bs >= qx and bx <= qx + bw and by + bs >= qy and by <= qy + bh then
            live[i] = false
            screen.hide(100 + i)
            vy = -vy
            sc = sc + 10
            hud()
            dropPowerup(qx, qy)
            device.beep()
            done = true
            break
          end
        end
      end
    end

    -- ball past the paddle = ball lost
    if by > sy then
      lives = lives - 1
      hud()
      if lives <= 0 then
        st = 1
        saveScore()
        msg('Game Over - tap to retry', 0xff453a)
      else
        st = 3
        waitUntil = now + 900
        msg('Ball lost!', 0xff9f0a)
      end
    end

    -- level cleared?
    local any = false
    for i = 0, rows * cols - 1 do if live[i] then any = true break end end
    if not any and st == 0 then
      st = 2
      waitUntil = now + 900
      sc = sc + 25
      saveScore()
      msg('Level up!', 0x30d158)
      device.beep(true)
    end
  end

  -- falling power-up
  if dtype ~= 0 then
    dpy = dpy + 2.2
    local letters = {'W', 'L', 'B'}
    local dcol = {0x0a84ff, 0x30d158, 0xffd60a}
    screen.box(30, dpx, math.floor(dpy), 16, 16, dcol[dtype])
    screen.label(31, dpx + 4, math.floor(dpy), letters[dtype], 0x000000)
    if dpy + 16 >= py and dpy <= py + ph and dpx + 16 >= px and dpx <= px + pw then
      applyPowerup()
    elseif dpy > sy then
      dtype = 0; screen.hide(30); screen.hide(31)
    end
  end

  screen.box(2, math.floor(px), py, pw, ph, 0x0a84ff)                    -- paddle
  if st == 0 then screen.box(1, math.floor(bx), math.floor(by), bs, bs, 0xffffff)
  else screen.hide(1) end
  screen.box(21, math.floor(px) + pw // 2 - 17, sy + 3, 34, 30, 0x0a84ff) -- strip knob
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
    UObj *s = findOrCreateSlot(id, 0);
    if (!s)
        return;
    lv_obj_t *o = s->obj;
    if (!text)
        text = "";
    // Games redraw everything every tick; only touch LVGL when something changed,
    // or every repeat call invalidates (repaints) the area for nothing.
    if (s->cp[0] == x && s->cp[1] == y && s->ccol == color && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) &&
        strcmp(lv_label_get_text(o), text) == 0)
        return;
    s->cp[0] = x;
    s->cp[1] = y;
    s->ccol = color;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
}

extern "C" void tdeck_ui_box(int id, int x, int y, int w, int h, uint32_t color, int radius)
{
    UObj *s = findOrCreateSlot(id, 1);
    if (!s)
        return;
    lv_obj_t *o = s->obj;
    if (s->cp[0] == x && s->cp[1] == y && s->cp[2] == w && s->cp[3] == h && s->cp[4] == radius && s->ccol == color &&
        !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
        return; // unchanged — e.g. redrawing all bumpers when only one flashed
    s->cp[0] = x;
    s->cp[1] = y;
    s->cp[2] = w;
    s->cp[3] = h;
    s->cp[4] = radius;
    s->ccol = color;
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0); // big radius + square size = a circle
}

// A thick line from (x1,y1) to (x2,y2) — angled bars for flippers, rails, lane
// guides. Rounded ends so it reads as a solid paddle. Points live in the slot so
// lv_line's retained pointer stays valid.
extern "C" void tdeck_ui_line(int id, int x1, int y1, int x2, int y2, int thickness, uint32_t color)
{
    UObj *s = findOrCreateSlot(id, 2);
    if (!s)
        return;
    if (s->cp[0] == x1 && s->cp[1] == y1 && s->cp[2] == x2 && s->cp[3] == y2 && s->cp[4] == thickness &&
        s->ccol == color && !lv_obj_has_flag(s->obj, LV_OBJ_FLAG_HIDDEN))
        return; // unchanged (a resting flipper redrawn every tick) — skip the repaint
    s->cp[0] = x1;
    s->cp[1] = y1;
    s->cp[2] = x2;
    s->cp[3] = y2;
    s->cp[4] = thickness;
    s->ccol = color;
    lv_obj_clear_flag(s->obj, LV_OBJ_FLAG_HIDDEN);
    // Size the object to the line's own bounding box (pad covers the rounded caps),
    // with the points relative to it — so moving a flipper repaints a small patch,
    // not the whole screen.
    int pad = thickness / 2 + 2;
    int ox = (x1 < x2 ? x1 : x2) - pad;
    int oy = (y1 < y2 ? y1 : y2) - pad;
    int w = (x1 > x2 ? x1 - x2 : x2 - x1) + pad * 2;
    int h = (y1 > y2 ? y1 - y2 : y2 - y1) + pad * 2;
    lv_obj_set_pos(s->obj, ox, oy);
    lv_obj_set_size(s->obj, w, h);
    s->pts[0].x = x1 - ox;
    s->pts[0].y = y1 - oy;
    s->pts[1].x = x2 - ox;
    s->pts[1].y = y2 - oy;
    lv_line_set_points(s->obj, s->pts, 2);
    lv_obj_set_style_line_width(s->obj, thickness, 0);
    lv_obj_set_style_line_color(s->obj, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(s->obj, true, 0);
}

extern "C" void tdeck_ui_hide(int id)
{
    for (int i = 0; i < uobjCount; i++)
        if (uobjs[i].id == id) {
            lv_obj_add_flag(uobjs[i].obj, LV_OBJ_FLAG_HIDDEN);
            return;
        }
}

// ---- app-folder file store (the store.* toolbox, jailed) ----------------------
// Reads /apps/<app>/<name> into buf; returns bytes read or -1.
extern "C" int tdeck_appfs_read(const char *name, char *buf, int cap)
{
    if (!appDir[0] || !storeNameOk(name) || !buf || cap <= 1)
        return -1;
    char path[104];
    snprintf(path, sizeof(path), "%s/%s", appDir, name);
    FsFile f = SDFs.open(path, O_RDONLY);
    if (!f)
        return -1;
    int n = f.read((uint8_t *)buf, cap - 1);
    f.close();
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

// Writes /apps/<app>/<name> (overwrite). Returns true on success.
extern "C" bool tdeck_appfs_write(const char *name, const char *data, int len)
{
    if (!appDir[0] || !storeNameOk(name) || !data || len < 0)
        return false;
    char path[104];
    snprintf(path, sizeof(path), "%s/%s", appDir, name);
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return false;
    int wrote = f.write((const uint8_t *)data, (size_t)len);
    f.sync();
    f.close();
    return wrote == len;
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
