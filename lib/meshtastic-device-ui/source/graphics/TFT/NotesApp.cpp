// -----------------------------------------------------------------------------
// T-UI Notes — a self-contained notes app for the launcher.
//
// Notes are plain .txt files in /notes on the SD card (created on first save;
// nothing is ever scanned outside that one small folder). Two screens:
//   • List — every note, newest first, previewed by its first line. "+ New"
//     starts a fresh note; tapping a row opens it.
//   • Editor — a full-screen text box typed into with the physical keyboard.
//     Back SAVES and returns to the list (a brand-new empty note is discarded);
//     Delete removes the note for good.
// The textarea is focused via LVGL's default input group (same way the mesh
// chat input gets typed into), with a periodic refocus guard because the
// trackball encoder can wander focus. Trackball double-click = Home still works.
// -----------------------------------------------------------------------------
#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
#include "graphics/common/SdCard.h" // SdFat instance (SDFs) shared with maps/files
#define NOTES_HAVE_SD 1
#else
#define NOTES_HAVE_SD 0
#endif

extern "C" void notes_open(void);
extern "C" void notes_open_file(const char *path); // Files app: open any .txt in the editor

namespace
{
const int kMaxNotes = 40;
const size_t kMaxNoteBytes = 4000;

lv_obj_t *listScreen = nullptr;
lv_obj_t *listPanel = nullptr; // scrollable column of note rows
lv_obj_t *editScreen = nullptr;
lv_obj_t *editArea = nullptr; // the textarea
lv_obj_t *editHint = nullptr; // top-middle label ("type away" / "view only")
lv_obj_t *deleteBtn = nullptr;
lv_obj_t *prevScreen = nullptr;
lv_timer_t *focusGuard = nullptr;

unsigned noteIds[kMaxNotes];
char notePreview[kMaxNotes][36];
int noteCount = 0;
unsigned curId = 0;        // note being edited (list mode)
bool curIsNew = false;     // brand-new note: discard if left empty
char curPath[96] = {};     // file behind the editor (works for any .txt, not just /notes)
bool curFromFiles = false; // opened via the Files app: Back returns there, no Delete
bool curReadOnly = false;  // file was too big to load fully: never save it back truncated
lv_obj_t *filesReturnScreen = nullptr;

void notePath(char *buf, size_t sz, unsigned id) { snprintf(buf, sz, "/notes/n%03u.txt", id); }

#if NOTES_HAVE_SD
// scan /notes (and only /notes) for n###.txt files; preview = first line
void scanNotes(void)
{
    noteCount = 0;
    FsFile dir = SDFs.open("/notes", O_RDONLY);
    if (!dir || !dir.isDirectory())
        return;
    FsFile entry;
    while ((entry = dir.openNextFile()) && noteCount < kMaxNotes) {
        char name[32];
        entry.getName(name, sizeof(name));
        unsigned id;
        if (!entry.isDirectory() && sscanf(name, "n%u.txt", &id) == 1) {
            char head[40] = {};
            int n = entry.read(head, sizeof(head) - 1);
            if (n < 0)
                n = 0;
            head[n] = 0;
            for (char *c = head; *c; c++)
                if (*c == '\r' || *c == '\n') {
                    *c = 0;
                    break;
                }
            noteIds[noteCount] = id;
            snprintf(notePreview[noteCount], sizeof(notePreview[0]), "%s", head[0] ? head : "(empty note)");
            noteCount++;
        }
        entry.close();
    }
    dir.close();
    // newest (highest id) first
    for (int i = 0; i < noteCount; i++)
        for (int j = i + 1; j < noteCount; j++)
            if (noteIds[j] > noteIds[i]) {
                unsigned t = noteIds[i];
                noteIds[i] = noteIds[j];
                noteIds[j] = t;
                char tmp[36];
                strcpy(tmp, notePreview[i]);
                strcpy(notePreview[i], notePreview[j]);
                strcpy(notePreview[j], tmp);
            }
}

unsigned nextFreeId(void)
{
    unsigned maxId = 0;
    for (int i = 0; i < noteCount; i++)
        if (noteIds[i] > maxId)
            maxId = noteIds[i];
    return maxId + 1;
}

// pull curPath into the textarea; flags read-only if the file didn't fit whole
void loadCurPathIntoEditor(void)
{
    static char buf[kMaxNoteBytes + 1];
    buf[0] = 0;
    curReadOnly = false;
    FsFile f = SDFs.open(curPath, O_RDONLY);
    if (f) {
        curReadOnly = f.size() > kMaxNoteBytes; // too big: show the start, never save back
        int n = f.read(buf, kMaxNoteBytes);
        if (n < 0)
            n = 0;
        buf[n] = 0;
        f.close();
    }
    lv_textarea_set_text(editArea, buf);
}

void saveCurrentNote(void)
{
    if (curReadOnly)
        return; // never write a truncated copy over the original
    const char *text = lv_textarea_get_text(editArea);
    if (curIsNew && (!text || !*text))
        return; // never-typed-in new note: don't create a file
    if (!curFromFiles)
        SDFs.mkdir("/notes");
    FsFile f = SDFs.open(curPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return;
    f.print(text ? text : "");
    f.close();
}

void deleteCurrentNote(void)
{
    if (SDFs.exists(curPath))
        SDFs.remove(curPath);
}
#else
void scanNotes(void) { noteCount = 0; }
unsigned nextFreeId(void) { return 1; }
void loadCurPathIntoEditor(void) { lv_textarea_set_text(editArea, ""); }
void saveCurrentNote(void) {}
void deleteCurrentNote(void) {}
#endif

// ---------------- UI ----------------
lv_obj_t *barBtn(lv_obj_t *parent, const char *txt, int w, lv_align_t align, int xofs, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 24);
    lv_obj_align(btn, align, xofs, 4);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    return btn;
}

void rebuildList(void);
void openEditor(unsigned id, bool isNew);

void buildListScreen(void)
{
    listScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(listScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(listScreen, LV_OBJ_FLAG_SCROLLABLE);

    barBtn(listScreen, "Back", 56, LV_ALIGN_TOP_LEFT, 4, 0x2c2c2e, [](lv_event_t *) {
        if (prevScreen)
            lv_screen_load_anim(prevScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    });

    lv_obj_t *title = lv_label_create(listScreen);
    lv_label_set_text(title, "Notes");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffd60a), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 9);

    barBtn(listScreen, "+ New", 64, LV_ALIGN_TOP_RIGHT, -4, 0x30d158,
           [](lv_event_t *) { openEditor(nextFreeId(), true); });

    listPanel = lv_obj_create(listScreen);
    lv_obj_remove_style_all(listPanel);
    lv_obj_set_pos(listPanel, 0, 32);
    lv_obj_set_size(listPanel, 320, 208);
    lv_obj_set_flex_flow(listPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(listPanel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(listPanel, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(listPanel, LV_DIR_VER);
}

void rebuildList(void)
{
    lv_obj_clean(listPanel);
    scanNotes();

#if NOTES_HAVE_SD
    if (noteCount == 0) {
        lv_obj_t *empty = lv_label_create(listPanel);
        lv_label_set_text(empty, "No notes yet - tap + New");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    }
#else
    lv_obj_t *empty = lv_label_create(listPanel);
    lv_label_set_text(empty, "Notes needs the SD card");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x8e8e93), LV_PART_MAIN);
#endif

    for (int i = 0; i < noteCount; i++) {
        lv_obj_t *row = lv_btn_create(listPanel);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                if (idx < noteCount)
                    openEditor(noteIds[idx], false);
            },
            LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, notePreview[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void buildEditScreen(void)
{
    editScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(editScreen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(editScreen, LV_OBJ_FLAG_SCROLLABLE);

    // Back = save + return to wherever the note was opened from (list or Files)
    barBtn(editScreen, "Back", 56, LV_ALIGN_TOP_LEFT, 4, 0x2c2c2e, [](lv_event_t *) {
        saveCurrentNote();
        if (curFromFiles && filesReturnScreen) {
            lv_screen_load_anim(filesReturnScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            return;
        }
        rebuildList();
        lv_screen_load_anim(listScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    });

    editHint = lv_label_create(editScreen);
    lv_label_set_text(editHint, "type away");
    lv_obj_set_style_text_color(editHint, lv_color_hex(0x8e8e93), LV_PART_MAIN);
    lv_obj_align(editHint, LV_ALIGN_TOP_MID, 0, 9);

    // only shown for notes opened from the list; Files has its own Trash button
    deleteBtn = barBtn(editScreen, "Delete", 64, LV_ALIGN_TOP_RIGHT, -4, 0xff453a, [](lv_event_t *) {
        deleteCurrentNote();
        rebuildList();
        lv_screen_load_anim(listScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    });

    editArea = lv_textarea_create(editScreen);
    lv_obj_set_pos(editArea, 0, 32);
    lv_obj_set_size(editArea, 320, 208);
    lv_obj_set_style_bg_color(editArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_text_color(editArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(editArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(editArea, 0, LV_PART_MAIN);
    // Visible caret. A blinking thin line proved unreliable — it only repainted
    // when the screen changed (so it seemed to appear only on Back). Use a SOLID,
    // non-blinking, semi-transparent white block over the character position: it's
    // clearly visible and doesn't depend on the blink timer firing a repaint.
    // (anim_duration 0 => LVGL keeps the cursor shown solid whenever focused.)
    lv_obj_set_style_bg_color(editArea, lv_color_hex(0xffffff), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(editArea, LV_OPA_50, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(editArea, 0, LV_PART_CURSOR);
    lv_textarea_set_max_length(editArea, kMaxNoteBytes);
    lv_textarea_set_placeholder_text(editArea, "Start typing on the keyboard...");
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), editArea);

    // keep the physical keyboard aimed at the textarea while the editor is up
    focusGuard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            if (g && lv_group_get_focused(g) != editArea)
                lv_group_focus_obj(editArea);
        },
        300, NULL);
    lv_timer_pause(focusGuard);

    lv_obj_add_event_cb(
        editScreen,
        [](lv_event_t *) {
            lv_timer_resume(focusGuard);
            if (lv_group_get_default())
                lv_group_focus_obj(editArea);
        },
        LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(
        editScreen, [](lv_event_t *) { lv_timer_pause(focusGuard); }, LV_EVENT_SCREEN_UNLOADED, NULL);
}

void openEditor(unsigned id, bool isNew)
{
    if (!editScreen)
        buildEditScreen();
    curId = id;
    curIsNew = isNew;
    curFromFiles = false;
    curReadOnly = false;
    notePath(curPath, sizeof(curPath), id);
    lv_obj_clear_flag(deleteBtn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(editHint, "type away");
    if (isNew)
        lv_textarea_set_text(editArea, "");
    else
        loadCurPathIntoEditor();
    lv_screen_load_anim(editScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
} // namespace

extern "C" void notes_open(void)
{
    lv_obj_t *active = lv_screen_active();
    if (active != listScreen && active != editScreen)
        prevScreen = active; // remember the launcher, not our own screens
    if (!listScreen)
        buildListScreen();
    rebuildList();
    lv_screen_load_anim(listScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// Files app hook: open any .txt on the SD card in the editor. Back saves and
// returns to the Files screen; oversized files open view-only (never saved back).
extern "C" void notes_open_file(const char *path)
{
#if NOTES_HAVE_SD
    if (!path || !*path)
        return;
    if (!editScreen)
        buildEditScreen();
    snprintf(curPath, sizeof(curPath), "%s", path);
    curId = 0;
    curIsNew = false;
    curFromFiles = true;
    filesReturnScreen = lv_screen_active();
    lv_obj_add_flag(deleteBtn, LV_OBJ_FLAG_HIDDEN);
    loadCurPathIntoEditor();
    lv_label_set_text(editHint, curReadOnly ? "view only (big file)" : "type away");
    lv_screen_load_anim(editScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
#else
    (void)path;
#endif
}
