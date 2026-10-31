// MHGU_PAUSE_PAGES=<n>: grow the in-hunt pause tab bar (uUIQuestStartMenu, class table
// 0x177aeb0, main_107/108.c) past its stock two pages (docs/UI.md 5b).
//
// The Options menu (cUIOOptionWindowForGame) is compacted into a one-page list: Gameplay, Audio,
// Controls, Graphics, Default Settings and Reset All Settings. The row/value virtuals remap each displayed row to its original
// stock entry, and the per-frame update moves the pager to that entry while an editor is open,
// so the engine still edits the right setting.
//
// Audio holds Music Volume and SFX Volume. Gameplay holds HUD, Map, Melody Effect Display and
// Lens Flares. Controls
// holds the 19 control-related entries in their stock order (3 pages). Graphics is the custom
// six-row menu: Display (Aspect Ratio / Resolution / Framerate / VSync), FXAA, MSAA, FSR, FSR
// Quality and FSR Sharpness. The count virtual (+0x60) reports 6 for the main list, 2 for Audio,
// 4 for Gameplay, 19 for Controls, 6 for Graphics and 4 for Display; the menu rows are drawn
// from the HUD slot (item 2) with their own labels.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../hle.h"
#include "../config.h"
#include "../platform.h"
#include "../textinput.h"
#include "input.h"

#define FN_BAR_INIT     0x877d0cu     // uUIQuestStartMenu init virtual (fills the paging struct)
#define BAR_VPTR_RT     0x197aeb8u    // uUIQuestStartMenu object vptr (class table 0x177aeb0 + 8)
#define PAGE_CNT        0xeau         // u16 page count in the bar's paging struct

#define FN_OPT_COUNT    0x7e7534u     // item-count virtual (+0x60)
#define FN_OPT_ROW      0x7e7558u     // label/layout virtual (+0x68)
#define FN_OPT_VALUE    0x7e7d34u     // current-value virtual (+0x6c)
#define FN_OPT_UPDATE   0x7e5840u     // per-frame window update virtual
#define FN_OPT_LAYOUT   0x7e635cu     // row layout
#define FN_PARAM_SET    0x85e5bcu     // festa parameter setter
#define OPT_VPTR_RT     0x1972c88u    // cUIOOptionWindowForGame object vptr

// The window's pager (embedded setup param at +0x4c): row +0xd (obj+0x59), rows/page +0xe
// (obj+0x5a), page u16 +0x10 (obj+0x5c), page count u16 +0x12 (obj+0x5e), total items u16 +0x14
// (obj+0x60). The window's edit state is +0x74 (2 while a value editor is open).
#define PW_ROW(w)   ((w) + 0x59u)
#define PW_ROWS(w)  ((w) + 0x5au)
#define PW_PAGE(w)  ((w) + 0x5cu)

#define STOCK_ITEMS  27u              // rows of the stock Options list
#define MAIN_ROWS    6u               // Gameplay, Audio, Controls, Graphics, Default Settings, Reset
#define GAMEPLAY_ITEM 0u              // the Gameplay row in the compact list
#define AUDIO_ITEM   1u               // the Audio row in the compact list
#define CTRL_ITEM    2u               // the Controls row in the compact list
#define GRAPHICS_ITEM 3u              // the Graphics row in the compact list
#define CTRL_ROWS    19u              // control-related stock entries, in the Controls submenu
#define AUDIO_ROWS   2u               // Music Volume, SFX Volume
#define GAMEPLAY_ROWS 4u              // HUD, Map, Melody Effect Display, Lens Flares
#define VALUE_ITEM   4u               // "Camera Angle": a long label slot with five value strings
#define PLAIN_ITEM   2u               // "HUD": a plain On/Off slot (draws the two menu rows)

// Graphics menu rows (page 0).
#define ROW_DISPLAY  0
#define ROW_FXAA     1
#define ROW_MSAA     2
#define ROW_FSR      3
#define ROW_FSRQ     4
#define ROW_SHARP    5
#define GFX_ROWS     6u

// Display submenu rows (its own one-page view).
#define DISP_ASPECT  0
#define DISP_VERT    1
#define DISP_FPS     2
#define DISP_VSYNC   3
#define DISP_ROWS    4u

// The borrowed "Camera Angle" slot holds five value strings at +13, +20, +27, +34, +41. The
// window copies the value text when it builds a row, so the five slots are rewritten with the
// current row's texts just before that row is built — each row has its own list.
static const char *const g_row_texts[GFX_ROWS][5] = {
    {NULL, NULL, NULL, NULL, NULL},        // Display: opens the nested menu, no value
    {"Off", "On", NULL, NULL, NULL},       // FXAA
    {"Off", "2x", "4x", "8x", NULL},       // MSAA
    {"Off", "FSR 3", "FSR 4", NULL, NULL}, // FSR
    {NULL, NULL, NULL, NULL, NULL},        // FSR Quality: the current label goes into two slots
    {NULL, NULL, NULL, NULL, NULL},        // FSR Sharpness: the current value goes into two slots
};
static const char *const g_row_names[GFX_ROWS] = {"Display", "FXAA", "MSAA", "FSR", "FSR Quality",
                                                  "FSR Sharpness"};
static const char *const g_disp_names[DISP_ROWS] = {"Aspect Ratio", "Resolution", "Framerate", "VSync"};
// Aspect ratios and vertical resolutions for the Display submenu. The width is computed from
// the two: width = round(height * ratio), made even.
static const char *const g_aspect_lbl[6] = {"4:3", "3:2", "16:10", "16:9", "21:9", "32:9"};
static const float g_aspect_val[6] = {4.0f / 3.0f, 3.0f / 2.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f};
static const int g_vert_vals[6] = {480, 720, 960, 1080, 1440, 2160};
// Compact Options list (non-control stock items, in stock order) and the Controls submenu's
// stock items. The mapping is applied to the row/value virtuals and around the editor update, so
// the engine still edits the original entry behind a moved row.
static const int8_t g_main_map[MAIN_ROWS] = {-1, -1, -1, -1, 25, 26};
static const uint8_t g_ctrl_map[CTRL_ROWS] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 20, 21, 22, 23, 24};
static const uint8_t g_audio_map[AUDIO_ROWS] = {0, 1};
static const uint8_t g_gameplay_map[GAMEPLAY_ROWS] = {2, 3, 15, 19};
static int main_orig(uint32_t f) { return f < MAIN_ROWS ? g_main_map[f] : -1; }

enum { SUB_NONE, SUB_CTRL, SUB_AUDIO, SUB_GAMEPLAY };
static int g_sub;                     // the stock-entry submenu currently open (SUB_*)
static int is_sub(void) { return g_sub != SUB_NONE; }
static uint32_t sub_rows(void)
{
    return g_sub == SUB_CTRL ? CTRL_ROWS : g_sub == SUB_AUDIO ? AUDIO_ROWS : g_sub == SUB_GAMEPLAY ? GAMEPLAY_ROWS : 0u;
}
static int sub_orig(uint32_t f)
{
    if (g_sub == SUB_CTRL) return f < CTRL_ROWS ? g_ctrl_map[f] : -1;
    if (g_sub == SUB_AUDIO) return f < AUDIO_ROWS ? g_audio_map[f] : -1;
    if (g_sub == SUB_GAMEPLAY) return f < GAMEPLAY_ROWS ? g_gameplay_map[f] : -1;
    return -1;
}
static const char *sub_name(void) { return g_sub == SUB_CTRL ? "Controls" : g_sub == SUB_AUDIO ? "Audio" : "Gameplay"; }

static const char *const g_fsrq_ini[5] = {"NativeAA", "Quality", "Balanced", "Performance", "UltraPerformance"};
static const char *const g_fsrq_lbl[5] = {"NativeAA", "Quality", "Balanced", "Perf", "UltraPerf"};

static uint32_t g_pages;              // forced bar page count (MHGU_PAUSE_PAGES; 0: leave alone)
static int g_gfx_menu;                // 1: the Graphics menu is the current view
static int g_remap_pass;              // inside the engine's stock-coordinate pass
static uint64_t g_nav_btn;            // the direction bits held this frame
static uint64_t g_btn_now;            // all bits the player holds this frame
// A one-frame synthetic direction used to make the engine refresh the description on view entry.
static int g_open_remap;              // A is opening a stock editor on the compact list
static int g_help_pend = -1;
static int g_help_pend_start;
static uint32_t g_help_pend_page, g_help_pend_row, g_help_pend_rows, g_help_pend_pages, g_help_pend_total;
static int g_help_item = -1;          // original item whose help is on screen
static void hk_helpbox(Cpu *c);
static int g_disp_menu;                // 1: the Display submenu is the current view
static uint32_t g_opt_obj;            // the Options window
static int g_refresh;                 // rebuild the row nodes on the next update

static uint64_t g_a_last;             // last A press timestamp seen
static uint64_t g_lr_last;            // last left/right press timestamp seen
static uint64_t g_b_last;             // last B press timestamp seen
static uint64_t g_plus_last;          // last Plus press timestamp seen
static int g_swallow_b;               // keep B from the engine until it is released

// Submenu row labels. Each row alternates between two buffers: the window only redraws a text
// node whose string pointer changed.
static uint32_t g_lbl[GFX_ROWS][2];
static uint32_t g_disp_lbl[DISP_ROWS][2];
static int g_lbl_cur;
static uint32_t g_tbl;                // borrowed slot (Camera Angle), patched while the menu shows
static uint32_t g_tbl_w[16];
static int g_fsrq_disp;               // which of the two value strings the FSR Quality row shows
static int g_fps_disp;                // which of the two value strings the Framerate row shows
static int g_ar_disp, g_rh_disp;      // the same, for the Display submenu's rows
static int g_num_edit;                // typing the Resolution height in the row
static char g_num_buf[8];
static uint64_t g_num_blink;
static int g_num_cursor = 1, g_num_disp;

// The menu rows' labels, and the HUD value string blanked while they are built.
static uint32_t g_gfx_lbl, g_ctrl_lbl, g_audio_lbl, g_gameplay_lbl;
static uint32_t g_blank_addr;
static uint8_t g_blank_byte;
static int g_blank;

// The Options pager is shrunk to the four-row Graphics view; remember its shape.
static int g_saved_rows;
static uint32_t g_saved_pages;
static uint32_t g_saved_total;
static uint32_t g_saved_page, g_saved_row;

// The controls view saves the compact Options list's pager.
static int g_sub_saved_rows;
static uint32_t g_sub_saved_pages, g_sub_saved_total, g_sub_saved_page, g_sub_saved_row;

// The Display view saves the Graphics view's pager (the Options pager is already saved above).
static int g_disp_saved_rows;
static uint32_t g_disp_saved_pages, g_disp_saved_total, g_disp_saved_page, g_disp_saved_row;

static void enter_menu(void);
static void leave_menu(void);

static void put(uint32_t a, const char *s)
{
    for (uint32_t i = 0; s[i]; i++) ST8(a + i, (uint8_t)s[i]);
    ST8(a + (uint32_t)strlen(s), 0u);
}

static void ensure_bufs(void)
{
    if (g_lbl[0][0]) return;
    for (int r = 0; r < (int)GFX_ROWS; r++) {
        g_lbl[r][0] = hle_alloc(64);
        g_lbl[r][1] = hle_alloc(64);
    }
    for (int r = 0; r < (int)DISP_ROWS; r++) {
        g_disp_lbl[r][0] = hle_alloc(64);
        g_disp_lbl[r][1] = hle_alloc(64);
    }
    g_gfx_lbl = hle_alloc(16);
    put(g_gfx_lbl, "Graphics");
    g_ctrl_lbl = hle_alloc(16);
    put(g_ctrl_lbl, "Controls");
    g_audio_lbl = hle_alloc(16);
    put(g_audio_lbl, "Audio");
    g_gameplay_lbl = hle_alloc(16);
    put(g_gameplay_lbl, "Gameplay");
}

static void restore_tbl(void)
{
    if (!g_tbl) return;
    for (int i = 0; i < 16; i++) ST32(g_tbl + (uint32_t)i * 4u, g_tbl_w[i]);
    g_tbl = 0;
}

static int msaa_index(int v) { return v >= 8 ? 3 : v >= 4 ? 2 : v >= 2 ? 1 : 0; }
static int msaa_value(int i) { static const int v[4] = {1, 2, 4, 8}; return v[i & 3]; }
static const char *msaa_text(int v) { return v >= 8 ? "8x" : v >= 4 ? "4x" : v >= 2 ? "2x" : "Off"; }

extern bool g_fsr_on;                 // runtime/nvn/vulkan/fsr.c: FSR started at boot and working

static int sharp_index(void)
{
    int i = (int)(g_config.fsr_sharpness * 10.0f + 0.5f) - 1;
    return i < 0 ? 0 : i > 9 ? 9 : i;
}
static float sharp_value(int i) { return (float)(i + 1) * 0.1f; }
static char g_sharp_buf[8];
static int g_sharp_disp;              // which of the two value strings the row shows
static const char *sharp_text(void)
{
    snprintf(g_sharp_buf, sizeof g_sharp_buf, "%.1f", (double)sharp_value(sharp_index()));
    return g_sharp_buf;
}

// Framerate (the INI allows any 30..240; the menu offers these eight). The row shows the actual
// setting, so a value loaded from the INI that is not in the list is still visible; stepping
// snaps to the nearest listed rate first.
static const int g_fps_vals[8] = {30, 40, 60, 72, 120, 144, 165, 240};
static char g_fps_buf[8];
static int fps_index(void)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 8; i++) {
        int d = g_config.fps > g_fps_vals[i] ? g_config.fps - g_fps_vals[i] : g_fps_vals[i] - g_config.fps;
        if (d < bd) bd = d, best = i;
    }
    return best;
}
static const char *fps_text(void)
{
    snprintf(g_fps_buf, sizeof g_fps_buf, "%d", g_config.fps);
    return g_fps_buf;
}

static int fsr_sel(void) { return !g_config.fsr ? 0 : g_config.fsr_upscaler == FSR_UP_4 ? 2 : 1; }
static const char *fsr_text(int sel) { return sel == 2 ? "FSR 4" : sel == 1 ? "FSR 3" : "Off"; }

// The Resolution view: the current aspect is the configured one, or the window's shape when
// AspectRatio is Auto; the vertical is the configured height. Both are matched to the lists.
static int aspect_index(void)
{
    float a = g_config.aspect_ratio;
    if (!(a > 0.0f)) a = (g_config.width > 0 && g_config.height > 0) ? (float)g_config.width / (float)g_config.height
                                                                     : 16.0f / 9.0f;
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 6; i++) {
        float d = a > g_aspect_val[i] ? a - g_aspect_val[i] : g_aspect_val[i] - a;
        if (d < bd) bd = d, best = i;
    }
    return best;
}
static int vert_index(void)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 6; i++) {
        int d = g_config.height > g_vert_vals[i] ? g_config.height - g_vert_vals[i] : g_vert_vals[i] - g_config.height;
        if (d < bd) bd = d, best = i;
    }
    return best;
}
static char g_vert_buf[8];
static const char *vert_text(void)
{
    snprintf(g_vert_buf, sizeof g_vert_buf, "%d", g_config.height);
    return g_vert_buf;
}
// Write the two choices to the INI as one Resolution (and keep AspectRatio in step, so a rounded
// width cannot drift the shape). The window and render targets are built at start-up.
static void res_apply(int aspect, int vert)
{
    int w = (int)((float)vert * g_aspect_val[aspect] + 0.5f);
    if (w & 1) w++;                       // (the config's fit_aspect also keeps the sizes even)
    g_config.width = w;
    g_config.height = vert;
    g_config.aspect_ratio = g_aspect_val[aspect];
    char buf[32];
    snprintf(buf, sizeof buf, "%dx%d", w, vert);
    config_save_key("Resolution", buf);
    config_save_key("AspectRatio", g_aspect_lbl[aspect]);
    LOG("[pausepages] Resolution -> %s (%s, takes effect on the next start)\n", buf, g_aspect_lbl[aspect]);
}

// Typing the Resolution height in the row: the physical keyboard types (runtime/textinput.c, digits
// only, four of them), the row shows the digits and a blinking cursor, Enter applies.
static void num_start(void)
{
    if (g_num_edit) return;
    g_num_edit = 1;
    g_num_buf[0] = 0;
    g_num_cursor = 1;
    g_num_disp = 0;
    g_num_blink = plat_now_ns();
    textinput_digits_only(true);
    textinput_begin(NULL, 4);
    g_refresh = 1;
    LOG("[pausepages] Resolution height entry: type 4 digits, Enter applies, Escape cancels\n");
}

static void num_end(void)
{
    if (!g_num_edit) return;
    g_num_edit = 0;
    textinput_end();
    textinput_digits_only(false);
    g_refresh = 1;
}

static void num_apply(void)
{
    int v = atoi(g_num_buf);
    if (v < 240 || v > 4320) {
        LOG("[pausepages] Resolution height %d out of range (240..4320)\n", v);
        return;
    }
    res_apply(aspect_index(), v);
}

// Write the row's label (host buffer) and its value strings (borrowed table).
static void fill_row(int disp_menu, int row)
{
    ensure_bufs();
    uint32_t b = (disp_menu ? g_disp_lbl : g_lbl)[row][g_lbl_cur];
    memset(G2H(b), 0, 64);
    put(b, disp_menu ? g_disp_names[row] : g_row_names[row]);
    if (disp_menu) {
        // Four rows with six/eight/three values: only the current text is written, into two slots.
        const char *t;
        if (row == DISP_ASPECT) {
            t = g_aspect_lbl[aspect_index()];
        } else if (row == DISP_VERT) {
            if (g_num_edit) {
                snprintf(g_vert_buf, sizeof g_vert_buf, "%s%s", g_num_buf, g_num_cursor ? "|" : "");
                t = g_vert_buf;
            } else {
                t = vert_text();
            }
        } else if (row == DISP_FPS) {
            t = fps_text();
        } else {
            t = g_config.vsync ? "On" : "Off";
        }
        if (g_tbl) {
            put(g_tbl + 13u, t);
            put(g_tbl + 20u, t);
        }
        return;
    }
    if (row == ROW_DISPLAY) {
        if (g_tbl) put(g_tbl + 13u, "");   // the row opens a submenu and shows no value
        return;
    }
    if (row == ROW_FSRQ) {
        // The names are longer than the borrowed slot's seven-byte strings, so only the current
        // one is written, into slots 0 and 4 (28 bytes apart, no overlap). Reporting 0 or 4
        // alternately makes the string pointer change, which is what redraws the row.
        const char *t = g_fsrq_lbl[g_config.fsr_quality];
        if (g_tbl) {
            put(g_tbl + 13u, t);
            put(g_tbl + 41u, t);
        }
        return;
    }
    if (row == ROW_SHARP) {
        // Ten steps do not fit the borrowed slot's five strings. The row reports value 0 or 1 and
        // both slots hold the current text, so every change flips the string pointer and redraws.
        const char *t = sharp_text();
        if (g_tbl) {
            put(g_tbl + 13u, t);
            put(g_tbl + 20u, t);
        }
        return;
    }
    for (int i = 0; i < 5 && g_row_texts[row][i]; i++) {
        const char *t = g_row_texts[row][i];
        if (g_tbl) put(g_tbl + 13u + (uint32_t)i * 7u, t);
    }
}

static void bar_init(Cpu *c) {
    if (g_gfx_menu) leave_menu();
    fnhook_call_original(c, FN_BAR_INIT);
    uint32_t bar = c->r[0];
    if (bar && LD32(bar) == BAR_VPTR_RT && g_pages) {
        ST16(bar + PAGE_CNT, g_pages);
        LOG("[pausepages] bar %08x: pages -> %u\n", bar, g_pages);
    }
}

// ---- Options window ----
static int is_opt(Cpu *c) { return LD32(c->r[0]) == OPT_VPTR_RT; }

static uint32_t opt_flat(uint32_t w)
{
    return (uint32_t)LD16(PW_PAGE(w)) * LD8(PW_ROWS(w)) + LD8(PW_ROW(w));
}

// Descriptions for the rows the stock option tables cannot describe: the two menu rows and the
// custom Graphics/Display rows. These are fillers; edit the strings to change the help box text.
enum {
    CH_MAIN_AUDIO, CH_MAIN_GAMEPLAY, CH_MAIN_CTRL, CH_MAIN_GFX,
    CH_GFX_DISPLAY, CH_GFX_FXAA, CH_GFX_MSAA, CH_GFX_FSR, CH_GFX_FSRQ, CH_GFX_SHARP,
    CH_DSP_ASPECT, CH_DSP_RES, CH_DSP_FPS, CH_DSP_VSYNC, CH_COUNT
};
static const char *const g_custom_help[CH_COUNT] = {
    "Custom Audio Description",
    "Custom Gameplay Description",
    "Custom Controls Description",
    "Custom Graphics Description",
    "Custom Display Description",
    "Custom FXAA Description",
    "Custom MSAA Description",
    "Custom FSR Description",
    "Custom FSR Quality Description",
    "Custom FSR Sharpness Description",
    "Custom Aspect Ratio Description",
    "Custom Resolution Description",
    "Custom Framerate Description",
    "Custom VSync Description",
};
static uint32_t g_custom_guest[CH_COUNT];
static uint32_t g_help_obj;            // the pause help box (captured from its text setter)
static int g_custom_cur = -1;

static int custom_help_alloc(Cpu *c)
{
    if (g_custom_guest[0]) return 1;
    uint32_t total = 0;
    for (int i = 0; i < CH_COUNT; i++) total += (uint32_t)strlen(g_custom_help[i]) + 1u;
    uint32_t s0 = c->r[0], s1 = c->r[1], s2 = c->r[2], s3 = c->r[3], s12 = c->r[12], s14 = c->r[14];
    c->r[0] = total;
    guest_call(c, guest_sym("malloc"));
    uint32_t p = c->r[0];
    c->r[0] = s0; c->r[1] = s1; c->r[2] = s2; c->r[3] = s3; c->r[12] = s12; c->r[14] = s14;
    if (!p) return 0;
    char *d = (char *)G2H(p);
    uint32_t off = 0;
    for (int i = 0; i < CH_COUNT; i++) {
        uint32_t n = (uint32_t)strlen(g_custom_help[i]) + 1u;
        g_custom_guest[i] = p + off;
        memcpy(d + off, g_custom_help[i], n);
        off += n;
    }
    LOG("[pausepages] custom help strings at %08x\n", p);
    return 1;
}

static int custom_help_index(uint32_t w)
{
    if (!w) return -1;
    if (g_disp_menu) {
        uint32_t r = LD8(PW_ROW(w));
        return r < 4u ? CH_DSP_ASPECT + (int)r : -1;
    }
    if (g_gfx_menu) {
        uint32_t r = LD8(PW_ROW(w));
        return r < 6u ? CH_GFX_DISPLAY + (int)r : -1;
    }
    if (is_sub()) return -1;
    uint32_t flat = opt_flat(w);
    if (flat == AUDIO_ITEM) return CH_MAIN_AUDIO;
    if (flat == GAMEPLAY_ITEM) return CH_MAIN_GAMEPLAY;
    if (flat == CTRL_ITEM) return CH_MAIN_CTRL;
    if (flat == GRAPHICS_ITEM) return CH_MAIN_GFX;
    return -1;
}

static void opt_update(Cpu *c)
{
    int remapped = 0;
    uint32_t w = c->r[0];
    // While an entry is being edited, the engine applies the change to the item under its own
    // cursor. Move the pager to the original stock entry for the call, then put the compact
    // shape back; the row/value virtuals draw the same entry again at its new position.
    int editing = w && LD32(w + 0x74u) == 2u;
    if (g_help_pend >= 0 && !g_remap_pass) {
        int orig = g_help_pend;
        int start = g_help_pend_start;
        g_help_pend = -1;
        g_pad_force = 0;
        if (w && LD32(w) == OPT_VPTR_RT && !editing && g_btn_now == 0) {
            ST8(PW_ROWS(w), 8u);
            ST16(w + 0x5eu, 4u);
            ST16(w + 0x60u, STOCK_ITEMS);
            ST16(PW_PAGE(w), (uint32_t)start / 8u);
            ST8(PW_ROW(w), (uint32_t)start % 8u);
            g_remap_pass = 1;
            fnhook_call_original(c, FN_OPT_UPDATE);   // consumes the synthetic press, lands on orig
            g_remap_pass = 0;
            ST8(PW_ROWS(w), (uint8_t)g_help_pend_rows);
            ST16(w + 0x5eu, g_help_pend_pages);
            ST16(w + 0x60u, g_help_pend_total);
            ST16(PW_PAGE(w), g_help_pend_page);
            ST8(PW_ROW(w), (uint8_t)g_help_pend_row);
            g_help_item = orig;
            g_refresh = 1;
            remapped = 1;
        }
    }
    if (!g_remap_pass && w && LD32(w) == OPT_VPTR_RT && (is_sub() || (!g_gfx_menu && !g_disp_menu)) && editing) {
        uint32_t page = LD16(PW_PAGE(w)), rows = LD8(PW_ROWS(w)), pages = LD16(w + 0x5eu), total = LD16(w + 0x60u);
        uint8_t row = LD8(PW_ROW(w));
        uint32_t flat = page * (rows ? rows : 1u) + row;
        int orig = is_sub() ? sub_orig(flat) : main_orig(flat);
        if (orig >= 0) {
            ST8(PW_ROWS(w), 8u);
            ST16(w + 0x5eu, 4u);
            ST16(w + 0x60u, STOCK_ITEMS);
            ST16(PW_PAGE(w), (uint32_t)orig / 8u);
            ST8(PW_ROW(w), (uint32_t)orig % 8u);
            g_remap_pass = 1;

            fnhook_call_original(c, FN_OPT_UPDATE);
            g_remap_pass = 0;
            ST8(PW_ROWS(w), rows);
            ST16(w + 0x5eu, pages);
            ST16(w + 0x60u, total);
            ST16(PW_PAGE(w), page);
            ST8(PW_ROW(w), row);
            remapped = 1;
            if (editing || orig != g_help_item) {   // refresh the help (and the editor) only when needed
                g_help_item = orig;
                g_refresh = 1;
            }
        }
    }
    if (!remapped && g_open_remap && !editing && !g_remap_pass && w && LD32(w) == OPT_VPTR_RT &&
        (is_sub() || (!g_gfx_menu && !g_disp_menu))) {
        uint32_t flat = opt_flat(w);
        int orig = is_sub() ? sub_orig(flat) : main_orig(flat);
        g_open_remap = 0;
        if (orig >= 0) {
            uint32_t page = LD16(PW_PAGE(w)), rows = LD8(PW_ROWS(w)), pages = LD16(w + 0x5eu), total = LD16(w + 0x60u);
            uint8_t row = LD8(PW_ROW(w));
            ST8(PW_ROWS(w), 8u);
            ST16(w + 0x5eu, 4u);
            ST16(w + 0x60u, STOCK_ITEMS);
            ST16(PW_PAGE(w), (uint32_t)orig / 8u);
            ST8(PW_ROW(w), (uint32_t)orig % 8u);
            g_remap_pass = 1;
            fnhook_call_original(c, FN_OPT_UPDATE);   // the editor opens on the original entry
            g_remap_pass = 0;
            ST8(PW_ROWS(w), rows);
            ST16(w + 0x5eu, pages);
            ST16(w + 0x60u, total);
            ST16(PW_PAGE(w), page);
            ST8(PW_ROW(w), row);
            g_help_item = orig;
            g_refresh = 1;
            remapped = 1;
        }
    }
    if (!remapped) fnhook_call_original(c, FN_OPT_UPDATE);
    // The engine keys the help text to the stock list index and only refreshes it while it
    // consumes a direction press. Run that stock step with the cursor one row/page before the
    // entry we actually show, so the press lands on it: the description then matches the row.
    if (!remapped && !g_remap_pass && w && LD32(w) == OPT_VPTR_RT &&
        (is_sub() || (!g_gfx_menu && !g_disp_menu)) && !editing) {
        uint32_t flat = opt_flat(w);
        int src = is_sub() ? sub_orig(flat) : main_orig(flat);
        if (src >= 0 && src != g_help_item) {
            uint32_t page = LD16(PW_PAGE(w)), rows = LD8(PW_ROWS(w)), pages = LD16(w + 0x5eu), total = LD16(w + 0x60u);
            uint8_t row = LD8(PW_ROW(w));
            int start = -1, can_now = 0;
            if (g_nav_btn & (1ull << BTN_DOWN))       { if ((src % 8u) > 0u) { start = src - 1; can_now = 1; } }
            else if (g_nav_btn & (1ull << BTN_UP))    { if ((src % 8u) < 7u) { start = src + 1; can_now = 1; } }
            else if (g_nav_btn & (1ull << BTN_RIGHT)) { if (src >= 8) { start = src - 8; can_now = 1; } }
            else if (g_nav_btn & (1ull << BTN_LEFT))  { if (src + 8 < STOCK_ITEMS) { start = src + 8; can_now = 1; } }
            if (can_now) {                            // reuse the held press for the stock help step
                ST8(PW_ROWS(w), 8u);
                ST16(w + 0x5eu, 4u);
                ST16(w + 0x60u, STOCK_ITEMS);
                ST16(PW_PAGE(w), (uint32_t)start / 8u);
                ST8(PW_ROW(w), (uint32_t)start % 8u);
                g_remap_pass = 1;
                fnhook_call_original(c, FN_OPT_UPDATE);
                g_remap_pass = 0;
                ST8(PW_ROWS(w), rows);
                ST16(w + 0x5eu, pages);
                ST16(w + 0x60u, total);
                ST16(PW_PAGE(w), page);
                ST8(PW_ROW(w), row);
                g_help_item = src;
                g_refresh = 1;
            } else if (g_btn_now == 0 && g_help_pend < 0) {
                // A boundary step (row 0/7, page 0/3, or a freshly opened view): arm one synthetic
                // row press for the next frame's pad read and run the stock help pass then.
                int s = (src % 8u) > 0u ? src - 1 : src + 1;
                if (s >= 0 && s < STOCK_ITEMS) {
                    g_help_pend = src;
                    g_help_pend_start = s;
                    g_help_pend_page = page;
                    g_help_pend_row = row;
                    g_help_pend_rows = rows;
                    g_help_pend_pages = pages;
                    g_help_pend_total = total;
                    g_pad_force = 1ull << ((src % 8u) > 0u ? BTN_DOWN : BTN_UP);
                }
            }
        }
    }
    // Rows without stock descriptions get our own text, set through the pause help box's setter.
    if (!remapped && !g_remap_pass && w && LD32(w) == OPT_VPTR_RT && !editing) {
        int ci = custom_help_index(w);
        if (ci < 0) {
            g_custom_cur = -1;
        } else if (ci != g_custom_cur && g_help_obj && custom_help_alloc(c)) {
            g_custom_cur = ci;
            uint32_t s0 = c->r[0], s1 = c->r[1], s2 = c->r[2], s3 = c->r[3], s12 = c->r[12], s14 = c->r[14];
            c->r[0] = g_help_obj;
            c->r[1] = g_custom_guest[ci];
            c->r[2] = 0;
            guest_call(c, 0x8431bcu);
            c->r[0] = s0; c->r[1] = s1; c->r[2] = s2; c->r[3] = s3; c->r[12] = s12; c->r[14] = s14;
        }
    }
    if (g_num_edit) {
        char buf[32];
        int chars;
        int ev = textinput_poll(buf, sizeof buf, &chars);
        if (ev == TI_CHANGED) {
            snprintf(g_num_buf, sizeof g_num_buf, "%s", buf);   // digits only, at most four
            g_num_disp ^= 1;
            g_num_cursor = 1;
            g_num_blink = plat_now_ns();
            g_refresh = 1;
        } else if (ev == TI_ENTER) {
            num_apply();
            num_end();
        } else if (ev == TI_CANCEL) {
            num_end();
        }
        uint64_t now = plat_now_ns();
        if (now - g_num_blink >= 500000000ull) {
            g_num_blink = now;
            g_num_cursor = !g_num_cursor;
            g_num_disp ^= 1;
            g_refresh = 1;
        }
    }
    if (g_refresh && g_opt_obj) {
        g_refresh = 0;
        c->r[0] = g_opt_obj;
        GuestFn layout = dispatch_lookup(FN_OPT_LAYOUT);
        if (layout) layout(c);
    }
}

// The row virtuals get (page, row within the page); the absolute item is page*rows+row.
static uint32_t row_abs(uint32_t w, uint32_t page, uint32_t row)
{
    uint32_t rows = LD8(w + 0x5au);
    return page * (rows ? rows : 1u) + row;
}

static void opt_row(Cpu *c)
{
    int on = is_opt(c);
    uint32_t item = c->r[3];
    uint32_t abs = on ? row_abs(c->r[0], c->r[2], item) : 0u;
    int menu_row = -1, sub_row = -1, orig = -1;
    if (on) {
        if (g_opt_obj != c->r[0]) {           // the window just opened: its opening press is not input
            g_opt_obj = c->r[0];
            g_a_last = g_a_pressed_ns;
            g_b_last = g_b_pressed_ns;
            g_plus_last = g_plus_pressed_ns;
            g_custom_cur = -1;
        }
        if (is_sub()) {
            if (!g_remap_pass) orig = sub_orig(abs);
        } else if (g_disp_menu) {
            sub_row = (int)item;              // 0 Aspect Ratio, 1 Resolution, 2 Framerate, 3 VSync
            c->r[3] = VALUE_ITEM;
        } else if (g_gfx_menu) {
            sub_row = (int)item;              // 0 Display, 1 FXAA ... 5 FSR Sharpness
            c->r[3] = VALUE_ITEM;
        } else if (!g_remap_pass && abs <= GRAPHICS_ITEM) {
            c->r[2] = 0u;                     // item 2 (HUD): a plain one-line row
            c->r[3] = PLAIN_ITEM;
            menu_row = (int)abs;
        } else if (!g_remap_pass) {
            orig = main_orig(abs);
        }
        if (orig >= 0) {                      // draw the original stock entry behind this row
            c->r[2] = (uint32_t)orig / 8u;
            c->r[3] = (uint32_t)orig % 8u;
        }
    }
    uint32_t out = c->r[1];
    if (g_blank) {                            // the previous row was one of the menu rows
        ST8(g_blank_addr, g_blank_byte);
        g_blank = 0;
    }
    fnhook_call_original(c, FN_OPT_ROW);
    if (!on || out < 0x0c000000u) return;
    uint32_t src = LD32(out);
    if (g_disp_menu || g_gfx_menu) {
        if (!g_tbl && src >= 0x10000000u && src < 0x1c000000u) {
            g_tbl = src;
            for (int i = 0; i < 16; i++) g_tbl_w[i] = LD32(src + (uint32_t)i * 4u);
        }
        if (g_disp_menu) {
            if (sub_row < 0 || sub_row >= (int)DISP_ROWS) sub_row = DISP_ASPECT;
        } else if (sub_row < 0 || sub_row >= (int)GFX_ROWS) {
            sub_row = ROW_DISPLAY;
        }
        fill_row(g_disp_menu, sub_row);         // its texts before the caller builds the value node
        ST32(out, (g_disp_menu ? g_disp_lbl : g_lbl)[sub_row][g_lbl_cur]);
    } else if (menu_row >= 0) {
        ensure_bufs();
        ST32(out, menu_row == AUDIO_ITEM ? g_audio_lbl : menu_row == GAMEPLAY_ITEM ? g_gameplay_lbl
                     : menu_row == CTRL_ITEM ? g_ctrl_lbl : g_gfx_lbl);
        if (src >= 0x10000000u && src < 0x1c000000u) {
            g_blank_addr = src + 4u;          // value 0 string ("On") -> empty for this row
            g_blank_byte = LD8(g_blank_addr);
            ST8(g_blank_addr, 0u);
            g_blank = 1;
        }
    }
}

static void opt_value(Cpu *c)
{
    if (is_opt(c)) {
        if (is_sub()) {
            if (!g_remap_pass) {
                int orig = sub_orig(row_abs(c->r[0], c->r[1], c->r[2]));
                if (orig >= 0) { c->r[1] = (uint32_t)orig / 8u; c->r[2] = (uint32_t)orig % 8u; }
            }
            fnhook_call_original(c, FN_OPT_VALUE);
            return;
        }
        if (g_disp_menu) {
            switch (c->r[2]) {
            case DISP_ASPECT: c->r[0] = (uint32_t)g_ar_disp; break;
            case DISP_VERT: c->r[0] = (uint32_t)(g_num_edit ? g_num_disp : g_rh_disp); break;
            case DISP_FPS: c->r[0] = (uint32_t)g_fps_disp; break;
            default: c->r[0] = g_config.vsync ? 1u : 0u; break;
            }
            return;
        }
        if (g_gfx_menu) {
            switch (c->r[2]) {                // the row is the item argument
            case ROW_DISPLAY: c->r[0] = 0u; break;
            case ROW_MSAA: c->r[0] = (uint32_t)msaa_index(g_config.msaa); break;
            case ROW_FSR: c->r[0] = (uint32_t)fsr_sel(); break;
            case ROW_FSRQ: c->r[0] = (uint32_t)g_fsrq_disp; break;
            case ROW_SHARP: c->r[0] = (uint32_t)g_sharp_disp; break;
            default: c->r[0] = g_config.fxaa ? 1u : 0u; break;
            }
            return;
        }
        uint32_t flat = row_abs(c->r[0], c->r[1], c->r[2]);
        if (!g_remap_pass) {
            if (flat <= GRAPHICS_ITEM) {
                c->r[0] = 0u;                 // the menu rows show no value
                return;
            }
            int orig = main_orig(flat);
            if (orig >= 0) { c->r[1] = (uint32_t)orig / 8u; c->r[2] = (uint32_t)orig % 8u; }
        }
    }
    fnhook_call_original(c, FN_OPT_VALUE);
}

static void opt_count(Cpu *c)
{
    if (is_opt(c)) {
        c->r[0] = is_sub() ? sub_rows() : g_disp_menu ? DISP_ROWS : g_gfx_menu ? GFX_ROWS : MAIN_ROWS;
        return;
    }
    fnhook_call_original(c, FN_OPT_COUNT);
}

static void param_set(Cpu *c)
{
    if (g_gfx_menu) return;                   // the submenu rows must not apply the borrowed option
    fnhook_call_original(c, FN_PARAM_SET);
}

static void enter_stock_menu(int which)
{
    if (is_sub()) return;
    g_sub = which;
    uint32_t w = g_opt_obj;
    if (w) {
        g_sub_saved_rows = LD8(PW_ROWS(w));
        g_sub_saved_pages = LD16(w + 0x5eu);
        g_sub_saved_total = LD16(w + 0x60u);
        g_sub_saved_page = LD16(PW_PAGE(w));
        g_sub_saved_row = LD8(PW_ROW(w));
        uint32_t rows = sub_rows();
        ST8(PW_ROW(w), 0u);
        ST16(PW_PAGE(w), 0u);
        ST8(PW_ROWS(w), 8u);
        ST16(w + 0x5eu, (uint16_t)((rows + 7u) / 8u));
        ST16(w + 0x60u, (uint16_t)rows);
        ST32(w + 0x74u, 0u);
    }
    g_lr_last = g_lr_pressed_ns;
    g_a_last = g_a_pressed_ns;
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_help_item = -1;
    g_open_remap = 0;
    g_help_pend = -1;
    g_pad_force = 0;
    g_custom_cur = -1;
    g_refresh = 1;
    LOG("[pausepages] %s menu\n", sub_name());
}

static void leave_stock_menu(void)
{
    if (!is_sub()) return;
    const char *name = sub_name();
    g_sub = SUB_NONE;
    uint32_t w = g_opt_obj;
    if (w) {
        ST8(PW_ROWS(w), g_sub_saved_rows ? (uint32_t)g_sub_saved_rows : 8u);
        ST16(w + 0x5eu, g_sub_saved_pages ? g_sub_saved_pages : 1u);
        ST16(w + 0x60u, g_sub_saved_total ? g_sub_saved_total : MAIN_ROWS);
        ST16(PW_PAGE(w), g_sub_saved_page);
        ST8(PW_ROW(w), (uint32_t)g_sub_saved_row);
        ST32(w + 0x74u, 0u);
    }
    g_lr_last = g_lr_pressed_ns;
    g_a_last = g_a_pressed_ns;
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_help_item = -1;
    g_open_remap = 0;
    g_help_pend = -1;
    g_pad_force = 0;
    g_custom_cur = -1;
    g_refresh = 1;
    LOG("[pausepages] %s menu -> Options list\n", name);
}

static void enter_disp_menu(void)
{
    if (g_disp_menu) return;
    g_disp_menu = 1;
    uint32_t w = g_opt_obj;
    if (w) {
        g_disp_saved_rows = LD8(PW_ROWS(w));
        g_disp_saved_pages = LD16(w + 0x5eu);
        g_disp_saved_total = LD16(w + 0x60u);
        g_disp_saved_page = LD16(PW_PAGE(w));
        g_disp_saved_row = LD8(PW_ROW(w));
        ST8(PW_ROW(w), 0u);
        ST16(PW_PAGE(w), 0u);
        ST8(PW_ROWS(w), DISP_ROWS);
        ST16(w + 0x5eu, 1u);
        ST16(w + 0x60u, DISP_ROWS);
        ST32(w + 0x74u, 0u);
    }
    g_lr_last = g_lr_pressed_ns;
    g_a_last = g_a_pressed_ns;
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_refresh = 1;
    LOG("[pausepages] Display menu\n");
}

static void leave_disp_menu(void)
{
    if (!g_disp_menu) return;
    num_end();
    g_disp_menu = 0;
    uint32_t w = g_opt_obj;
    if (w) {
        ST8(PW_ROWS(w), g_disp_saved_rows ? (uint32_t)g_disp_saved_rows : GFX_ROWS);
        ST16(w + 0x5eu, g_disp_saved_pages ? g_disp_saved_pages : 1u);
        ST16(w + 0x60u, g_disp_saved_total ? g_disp_saved_total : GFX_ROWS);
        ST16(PW_PAGE(w), g_disp_saved_page);
        ST8(PW_ROW(w), (uint32_t)g_disp_saved_row);
        ST32(w + 0x74u, 0u);
    }
    g_lr_last = g_lr_pressed_ns;
    g_a_last = g_a_pressed_ns;
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_refresh = 1;
    LOG("[pausepages] Display menu -> Graphics menu\n");
}

static void enter_menu(void)
{
    if (g_gfx_menu) return;
    g_gfx_menu = 1;
    uint32_t w = g_opt_obj;
    if (w) {
        g_saved_rows = LD8(PW_ROWS(w));
        g_saved_pages = LD16(w + 0x5eu);
        g_saved_total = LD16(w + 0x60u);      // +0x14 of the paging struct: total items
        g_saved_page = LD16(PW_PAGE(w));
        g_saved_row = LD8(PW_ROW(w));
        ST8(PW_ROW(w), 0u);
        ST16(PW_PAGE(w), 0u);
        ST8(PW_ROWS(w), GFX_ROWS);
        ST16(w + 0x5eu, 1u);
        ST16(w + 0x60u, GFX_ROWS);
        ST32(w + 0x74u, 0u);                  // leave the borrowed slot's edit state
    }
    g_lr_last = g_lr_pressed_ns;              // the presses so far are not input for this view
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_help_item = -1;
    g_open_remap = 0;
    g_help_pend = -1;
    g_pad_force = 0;
    g_custom_cur = -1;
    g_refresh = 1;
    LOG("[pausepages] Graphics menu\n");
}

static void leave_menu(void)
{
    if (!g_gfx_menu) return;
    num_end();
    g_gfx_menu = 0;
    g_disp_menu = 0;
    restore_tbl();
    uint32_t w = g_opt_obj;
    if (w) {
        ST8(PW_ROWS(w), g_saved_rows ? (uint32_t)g_saved_rows : 8u);
        ST16(w + 0x5eu, g_saved_pages ? g_saved_pages : 4u);
        ST16(w + 0x60u, g_saved_total ? g_saved_total : STOCK_ITEMS + 1u);
        ST16(PW_PAGE(w), g_saved_page);
        ST8(PW_ROW(w), (uint32_t)g_saved_row);
        ST32(w + 0x74u, 0u);
    }
    g_a_last = g_a_pressed_ns;                // presses made inside the menu are not list input
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_lr_last = g_lr_pressed_ns;
    g_help_item = -1;
    g_open_remap = 0;
    g_help_pend = -1;
    g_pad_force = 0;
    g_custom_cur = -1;
    g_refresh = 1;
    LOG("[pausepages] Graphics menu -> Options list\n");
}

// The FSR upscaler (and the render resolution it works from) is built at start-up when FSR is on.
// The setting is always written to the INI; live, FSR can be switched off and back on when it was
// started at boot, but enabling it from a non-FSR boot (or changing 3 <-> 4) needs the next start.
static int g_fsr_boot = -1, g_fsr_boot_up = -1;

static void fsr_apply(int sel)
{
    if (g_fsr_boot < 0) { g_fsr_boot = g_fsr_on; g_fsr_boot_up = g_config.fsr_upscaler; }
    g_config.fsr = sel != 0;
    g_config.fsr_upscaler = sel == 2 ? FSR_UP_4 : FSR_UP_3;
    config_save_key("FSR", g_config.fsr ? "On" : "Off");
    config_save_key("Upscaler", sel == 2 ? "FSR4" : "FSR3");
    if (!g_config.fsr) {
        g_fsr_on = false;
    } else if (g_fsr_boot && g_fsr_boot_up == g_config.fsr_upscaler) {
        g_fsr_on = true;                  // the upscaler built at start-up is used again
    } else {
        LOG("[pausepages] FSR %s takes effect on the next start (the upscaler is built at start-up)\n", fsr_text(sel));
    }
    LOG("[pausepages] FSR -> %s\n", fsr_text(sel));
}

// FSR Quality sets the render scale, which is fixed when the game's render targets are created at
// start-up; the menu writes the INI and the change applies on the next start.
static void fsrq_apply(int i)
{
    g_config.fsr_quality = i;
    config_save_key("FSRQuality", g_fsrq_ini[i]);
    LOG("[pausepages] FSR Quality -> %s%s\n", g_fsrq_ini[i],
        g_fsr_on ? " (takes effect on the next start: the render scale is set at start-up)" : "");
}

void pausepages_input(uint64_t *buttons)
{
    g_btn_now = *buttons;
    g_nav_btn = g_btn_now & ((1ull << BTN_LEFT) | (1ull << BTN_RIGHT) | (1ull << BTN_UP) | (1ull << BTN_DOWN));
    if (g_swallow_b) {                        // the B that left the menu is still held
        if (*buttons & (1ull << BTN_B)) *buttons &= ~(1ull << BTN_B);
        else g_swallow_b = 0;
    }
    if (!g_gfx_menu && !is_sub() && g_opt_obj) {
        // Options list. B/Plus leave it (let the engine close it) and the window handle is
        // dropped so the bar's own A presses are never mistaken for the extra row.
        if (g_b_pressed_ns != g_b_last) {
            g_b_last = g_b_pressed_ns;
            if (g_opt_obj && LD32(g_opt_obj + 0x74u) == 2u) return;  // editor: the engine cancels it
            g_opt_obj = 0;
            return;
        }
        if (g_plus_pressed_ns != g_plus_last) {
            g_plus_last = g_plus_pressed_ns;
            g_opt_obj = 0;
            return;
        }
        // A on the two menu rows opens them instead of the stock action (item 27 runs "Reset All
        // Settings"), so the press never reaches the engine.
        if (g_a_pressed_ns != g_a_last) {
            g_a_last = g_a_pressed_ns;
            uint32_t flat = opt_flat(g_opt_obj);
            if (flat == AUDIO_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_stock_menu(SUB_AUDIO);
            } else if (flat == GAMEPLAY_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_stock_menu(SUB_GAMEPLAY);
            } else if (flat == CTRL_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_stock_menu(SUB_CTRL);
            } else if (flat == GRAPHICS_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_menu();
            } else if (main_orig(flat) >= 0 && (!g_opt_obj || LD32(g_opt_obj + 0x74u) != 2u)) {
                g_open_remap = 1;
            }
        }
        return;
    }
    if (!g_gfx_menu && !is_sub()) return;
    // Safety net: if the window went away by some path other than B/Plus, stop touching input.
    static int hidden;
    if (g_opt_obj && !LD8(g_opt_obj + 0x18u)) {
        if (++hidden > 60) {
            hidden = 0;
            if (is_sub()) leave_stock_menu();
            leave_menu();
            return;
        }
    } else {
        hidden = 0;
    }
    if (is_sub()) {
        // The stock entries' editor runs in the engine (A, left/right, up/down all pass through;
        // the update hook moves its pager to the original entry). B closes the editor first.
        if (g_b_pressed_ns != g_b_last) {
            g_b_last = g_b_pressed_ns;
            if (g_opt_obj && LD32(g_opt_obj + 0x74u) == 2u) return;
            *buttons &= ~(1ull << BTN_B);
            g_swallow_b = 1;
            leave_stock_menu();
            return;
        }
        if (g_plus_pressed_ns != g_plus_last) {
            g_plus_last = g_plus_pressed_ns;
            leave_stock_menu();
            return;
        }
        if (!(g_opt_obj && LD32(g_opt_obj + 0x74u) == 2u) && g_a_pressed_ns != g_a_last) {
            g_a_last = g_a_pressed_ns;
            g_open_remap = 1;                 // A is about to open the stock editor
        }
        return;
    }
    if (g_num_edit) {                         // typing the height: the game gets no input at all
        *buttons = 0;
        if (g_a_pressed_ns != g_a_last) {
            g_a_last = g_a_pressed_ns;
            num_apply();
            num_end();
            return;
        }
        if (g_b_pressed_ns != g_b_last) {
            g_b_last = g_b_pressed_ns;
            num_end();                        // cancel
            return;
        }
        if (g_plus_pressed_ns != g_plus_last) {
            g_plus_last = g_plus_pressed_ns;
            num_end();
            leave_menu();
            return;
        }
        return;
    }
    *buttons &= ~(1ull << BTN_B);             // B belongs to this menu, not to the Options window
    if (g_b_pressed_ns != g_b_last) {
        g_b_last = g_b_pressed_ns;
        g_swallow_b = 1;
        if (g_disp_menu) leave_disp_menu();
        else leave_menu();
        return;
    }
    if (g_plus_pressed_ns != g_plus_last) {
        g_plus_last = g_plus_pressed_ns;
        leave_menu();
        return;
    }
    if (g_disp_menu && g_a_pressed_ns != g_a_last) {
        g_a_last = g_a_pressed_ns;
        int row = g_opt_obj ? (int)LD8(PW_ROW(g_opt_obj)) : DISP_ASPECT;
        if (row == DISP_VERT) {
            *buttons &= ~(1ull << BTN_A);
            num_start();
            return;
        }
    }
    // A on the Display row opens the nested view; on the other rows it stays the stock editor.
    if (!g_disp_menu && g_a_pressed_ns != g_a_last) {
        g_a_last = g_a_pressed_ns;
        int row = g_opt_obj ? (int)LD8(PW_ROW(g_opt_obj)) : ROW_DISPLAY;
        if (row == ROW_DISPLAY) {
            *buttons &= ~(1ull << BTN_A);
            enter_disp_menu();
            return;
        }
    }
    int dir = (*buttons & (1ull << BTN_RIGHT)) ? 1 : (*buttons & (1ull << BTN_LEFT)) ? -1 : 0;
    // The stock window still edits the item under its own cursor (row 0 is Music Volume) when
    // left/right arrives, even though the row virtual draws our rows; consume the press like B.
    *buttons &= ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT));
    if (g_lr_pressed_ns != g_lr_last) {
        g_lr_last = g_lr_pressed_ns;
        int row = g_opt_obj ? (int)LD8(PW_ROW(g_opt_obj)) : ROW_DISPLAY;
        if (!dir) return;
        if (g_disp_menu) {
            if (row == DISP_ASPECT) {
                int a = (aspect_index() + (dir > 0 ? 1 : 5)) % 6;
                res_apply(a, g_vert_vals[vert_index()]);
                g_ar_disp ^= 1;
            } else if (row == DISP_FPS) {
                void vi_set_framerate(int);
                int i = (fps_index() + (dir > 0 ? 1 : 7)) % 8;
                char buf[8];
                g_config.fps = g_fps_vals[i];
                vi_set_framerate(g_config.fps);
                g_fps_disp ^= 1;
                snprintf(buf, sizeof buf, "%d", g_config.fps);
                config_save_key("FPS", buf);
                LOG("[pausepages] Framerate -> %s\n", buf);
            } else if (row == DISP_VSYNC) {
                void vk_set_vsync(int);
                g_config.vsync = !g_config.vsync;
                vk_set_vsync(g_config.vsync);
                config_save_key("VSync", g_config.vsync ? "On" : "Off");
                LOG("[pausepages] VSync -> %s\n", g_config.vsync ? "On" : "Off");
            } else {
                return;                        // Resolution is typed, not stepped
            }
            g_lbl_cur ^= 1;
            g_refresh = 1;
            return;
        }
        switch (row) {
        case ROW_DISPLAY:
            return;                            // the nested Display view has its own rows
        case ROW_MSAA: {
            int i = (msaa_index(g_config.msaa) + (dir > 0 ? 1 : 3)) & 3;
            g_config.msaa = msaa_value(i);
            void vk_set_msaa(uint32_t);
            vk_set_msaa((uint32_t)g_config.msaa);
            config_save_key("MSAA", msaa_text(g_config.msaa));
            LOG("[pausepages] MSAA -> %s\n", msaa_text(g_config.msaa));
            break;
        }
        case ROW_FSR: {
            int i = (fsr_sel() + (dir > 0 ? 1 : 2)) % 3;
            fsr_apply(i);
            break;
        }
        case ROW_FSRQ: {
            int i = (g_config.fsr_quality + (dir > 0 ? 1 : 4)) % 5;
            g_fsrq_disp = g_fsrq_disp ? 0 : 4;
            fsrq_apply(i);
            break;
        }
        case ROW_SHARP: {
            int i = (sharp_index() + (dir > 0 ? 1 : 9)) % 10;
            g_config.fsr_sharpness = sharp_value(i);   // read by the FSR dispatch every frame
            g_sharp_disp ^= 1;
            config_save_key("Sharpness", sharp_text());
            LOG("[pausepages] FSR Sharpness -> %s\n", sharp_text());
            break;
        }
        default:
            g_config.fxaa = !g_config.fxaa;
            config_save_key("FXAA", g_config.fxaa ? "On" : "Off");
            LOG("[pausepages] FXAA -> %s\n", g_config.fxaa ? "On" : "Off");
            break;
        }
        g_lbl_cur ^= 1;                       // the label pointer must change for the row to redraw
        g_refresh = 1;
    }
}

// The pause help box's text setter: capture its object once so the custom rows can set their
// own descriptions through the same path.
static void hk_helpbox(Cpu *c)
{
    uint32_t t = c->r[1];
    if (!g_help_obj && t >= 0x1827a000u && t < 0x1827e000u) g_help_obj = c->r[0];
    // Keep the custom description when the engine refreshes the help during a value change.
    if (g_opt_obj && LD32(g_opt_obj) == OPT_VPTR_RT && !is_sub() && g_custom_guest[0]) {
        int ci = custom_help_index(g_opt_obj);
        if (ci >= 0) c->r[1] = g_custom_guest[ci];
    }
    fnhook_call_original(c, 0x8431bcu);
}

void pausepages_init(void)
{
    fnhook_set(0x8431bcu, hle_register("pausepages:helpbox", hk_helpbox));
    const char *e = getenv("MHGU_PAUSE_PAGES");
    if (e && *e) g_pages = (uint32_t)strtoul(e, NULL, 10);
    // The Graphics row/menu is always enabled; the bar-init hook is needed so a pause rebuild
    // drops the Graphics view (MHGU_PAUSE_PAGES only grows the bar's page count).
    fnhook_set(FN_BAR_INIT, hle_register("pausepages:bar_init", bar_init));
    fnhook_set(FN_OPT_COUNT, hle_register("pausepages:opt_count", opt_count));
    fnhook_set(FN_OPT_ROW, hle_register("pausepages:opt_row", opt_row));
    fnhook_set(FN_OPT_VALUE, hle_register("pausepages:opt_value", opt_value));
    fnhook_set(FN_OPT_UPDATE, hle_register("pausepages:opt_update", opt_update));
    fnhook_set(FN_PARAM_SET, hle_register("pausepages:param_set", param_set));
    LOG("[pausepages] Options: Graphics row + Display/FXAA/MSAA/FSR/FSR Quality/FSR Sharpness menu\n");
}
