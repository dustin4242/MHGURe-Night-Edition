// MHGU_PAUSE_PAGES=<n>: grow the in-hunt pause tab bar (uUIQuestStartMenu, class table
// 0x177aeb0, main_107/108.c) past its stock two pages (docs/UI.md 5b).
//
// MHGU_PAUSE_GRAPHICS=<0/1>: add a "Graphics" entry at the end of the Options menu
// (cUIOOptionWindowForGame). It opens a three-row Graphics menu: FXAA (Off/On), MSAA
// (Off/2x/4x/8x) and FSR (Off / FSR 3 / FSR 4). The stock options keep their own rows and indices.
//
// The window reports its item count through virtual +0x60 (f_7e7534, stock 27); reporting 28
// gives the list one extra row on its last page. That row is drawn from the HUD slot (item 2,
// a plain On/Off row) with the label "Graphics" and an empty value. Selecting it switches this
// same window to the two-row Graphics view.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
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
#define GRAPHICS_ITEM STOCK_ITEMS     // the extra row (last)
#define VALUE_ITEM   4u               // "Camera Angle": a long label slot with five value strings
#define FSR_ITEM     5u               // "Camera Controls": four value strings (Offset/Flip Y/Flip X/Flip X+Y)
#define PLAIN_ITEM   2u               // "HUD": a plain On/Off slot (draws the extra list row)

// Graphics submenu rows (page 0, rows 0/1).
#define ROW_FXAA     0
#define ROW_MSAA     1
#define ROW_FSR      2
#define GFX_ROWS     3u

// The borrowed "Camera Angle" slot holds five value strings at +13, +20, +27, +34, +41. Both
// submenu rows read their value text from there, so the five slots are shared:
//   0 "Off" (FXAA off, MSAA off)   1 "2x"   2 "On" (FXAA on)   3 "4x"   4 "8x"
#define SLOT_OFF     0
#define SLOT_2X      1
#define SLOT_ON      2
#define SLOT_4X      3
#define SLOT_8X      4
// Item 5's value strings at +16/+23/+30 (from its label): FSR = Off / FSR 3 / FSR 4.
#define FSR_OFF      0
#define FSR_3        1
#define FSR_4        2

static uint32_t g_pages;              // forced bar page count (MHGU_PAUSE_PAGES; 0: leave alone)
static int g_graphics;                // 1: Graphics row / menu enabled
static int g_gfx_menu;                // 1: the Graphics menu is the current view
static uint32_t g_opt_obj;            // the Options window
static int g_refresh;                 // rebuild the row nodes on the next update

static uint64_t g_a_last;             // last A press timestamp seen
static uint64_t g_lr_last;            // last left/right press timestamp seen
static uint64_t g_b_last;             // last B press timestamp seen
static uint64_t g_plus_last;          // last Plus press timestamp seen
static int g_swallow_b;               // keep B from the engine until it is released

// Submenu row labels. Each row alternates between two buffers: the window only redraws a text
// node whose string pointer changed. The value strings are looked up in the option table, so the
// "Type N" strings of the borrowed slot are rewritten while the Graphics menu shows.
static uint32_t g_lbl_fx[2], g_lbl_ms[2], g_lbl_fsr[2];
static int g_lbl_cur;
static uint32_t g_tbl;                // borrowed slot (Camera Angle), patched while the menu shows
static uint32_t g_tbl_w[16];
static uint32_t g_tbl_fsr;            // borrowed slot (Camera Controls), patched while the menu shows
static uint32_t g_tbl_fsr_w[16];

// "Graphics" list row label, and the HUD value string blanked for that row only.
static uint32_t g_gfx_lbl;
static uint32_t g_blank_addr;
static uint8_t g_blank_byte;
static int g_blank;

// The Options pager is shrunk to the two-row Graphics view; remember its shape.
static int g_saved_rows;
static uint32_t g_saved_pages;
static uint32_t g_saved_total;
static uint32_t g_saved_page, g_saved_row;

static void enter_menu(void);
static void leave_menu(void);

static void put(uint32_t a, const char *s)
{
    for (uint32_t i = 0; s[i]; i++) ST8(a + i, (uint8_t)s[i]);
    ST8(a + (uint32_t)strlen(s), 0u);
}

static void ensure_bufs(void)
{
    if (g_lbl_fx[0]) return;
    g_lbl_fx[0] = hle_alloc(64);
    g_lbl_fx[1] = hle_alloc(64);
    g_lbl_ms[0] = hle_alloc(64);
    g_lbl_ms[1] = hle_alloc(64);
    g_lbl_fsr[0] = hle_alloc(64);
    g_lbl_fsr[1] = hle_alloc(64);
    g_gfx_lbl = hle_alloc(16);
    put(g_gfx_lbl, "Graphics");
}

static void restore_tbl(void)
{
    if (g_tbl) {
        for (int i = 0; i < 16; i++) ST32(g_tbl + (uint32_t)i * 4u, g_tbl_w[i]);
        g_tbl = 0;
    }
    if (g_tbl_fsr) {
        for (int i = 0; i < 16; i++) ST32(g_tbl_fsr + (uint32_t)i * 4u, g_tbl_fsr_w[i]);
        g_tbl_fsr = 0;
    }
}

extern bool g_fsr_on;                 // runtime/nvn/vulkan/fsr.c: FSR started at boot and working

static int fsr_sel(void) { return !g_config.fsr ? FSR_OFF : g_config.fsr_upscaler == FSR_UP_4 ? FSR_4 : FSR_3; }
static const char *fsr_text(int sel) { return sel == FSR_4 ? "FSR 4" : sel == FSR_3 ? "FSR 3" : "Off"; }

static int msaa_index(int v) { return v >= 8 ? 3 : v >= 4 ? 2 : v >= 2 ? 1 : 0; }
static int msaa_value(int i) { static const int v[4] = {1, 2, 4, 8}; return v[i & 3]; }
static const char *msaa_text(int v) { return v >= 8 ? "8x" : v >= 4 ? "4x" : v >= 2 ? "2x" : "Off"; }
// The shared slot that shows the MSAA value (Off, 2x, 4x, 8x).
static int msaa_slot(int v) { return v >= 8 ? SLOT_8X : v >= 4 ? SLOT_4X : v >= 2 ? SLOT_2X : SLOT_OFF; }

// Write both submenu labels and the shared value strings into the host buffers and the table.
static void fill_lbl(void)
{
    ensure_bufs();
    static const char *const slot_text[5] = {"Off", "2x", "On", "4x", "8x"};
    uint32_t fx = g_lbl_fx[g_lbl_cur], ms = g_lbl_ms[g_lbl_cur], fs = g_lbl_fsr[g_lbl_cur];
    memset(G2H(fx), 0, 64);
    memset(G2H(ms), 0, 64);
    memset(G2H(fs), 0, 64);
    put(fx, "FXAA");
    put(ms, "MSAA");
    put(fs, "FSR");
    for (int i = 0; i < 5; i++) {
        put(fx + 13u + (uint32_t)i * 7u, slot_text[i]);
        put(ms + 13u + (uint32_t)i * 7u, slot_text[i]);
    }
    if (g_tbl) {
        put(g_tbl, "FXAA");                       // the label comes from the host buffer
        for (int i = 0; i < 5; i++) put(g_tbl + 13u + (uint32_t)i * 7u, slot_text[i]);
    }
    if (g_tbl_fsr) {                              // "Camera Controls" -> Off / FSR 3 / FSR 4
        put(g_tbl_fsr, "FSR");
        put(g_tbl_fsr + 16u, "Off");
        put(g_tbl_fsr + 23u, "FSR 3");
        put(g_tbl_fsr + 30u, "FSR 4");
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

static void opt_update(Cpu *c)
{
    fnhook_call_original(c, FN_OPT_UPDATE);
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
    int on = g_graphics && is_opt(c);
    uint32_t item = c->r[3];
    uint32_t abs = on ? row_abs(c->r[0], c->r[2], item) : 0u;
    int extra_row = 0;
    int sub_row = -1;
    if (on) {
        if (g_opt_obj != c->r[0]) {           // the window just opened: its opening press is not input
            g_opt_obj = c->r[0];
            g_a_last = g_a_pressed_ns;
            g_b_last = g_b_pressed_ns;
            g_plus_last = g_plus_pressed_ns;
        }
        if (g_gfx_menu) {
            sub_row = (int)item;              // 0: FXAA, 1: MSAA, 2: FSR
            c->r[3] = sub_row == ROW_FSR ? FSR_ITEM : VALUE_ITEM;
        } else if (abs == GRAPHICS_ITEM) {
            c->r[3] = PLAIN_ITEM;             // draw the Graphics row from the HUD slot
            extra_row = 1;
        }
    }
    uint32_t out = c->r[1];
    if (g_blank) {                            // the previous row was the Graphics row
        ST8(g_blank_addr, g_blank_byte);
        g_blank = 0;
    }
    fnhook_call_original(c, FN_OPT_ROW);
    if (!on || out < 0x0c000000u) return;
    uint32_t src = LD32(out);
    if (g_gfx_menu) {
        if (src >= 0x10000000u && src < 0x1c000000u) {
            if (sub_row == ROW_FSR) {
                if (!g_tbl_fsr) {
                    g_tbl_fsr = src;
                    for (int i = 0; i < 16; i++) g_tbl_fsr_w[i] = LD32(src + (uint32_t)i * 4u);
                }
            } else if (!g_tbl) {
                g_tbl = src;
                for (int i = 0; i < 16; i++) g_tbl_w[i] = LD32(src + (uint32_t)i * 4u);
            }
        }
        fill_lbl();
        ST32(out, sub_row == ROW_FSR ? g_lbl_fsr[g_lbl_cur]
                 : sub_row == ROW_MSAA ? g_lbl_ms[g_lbl_cur] : g_lbl_fx[g_lbl_cur]);
    } else if (extra_row) {
        ensure_bufs();
        ST32(out, g_gfx_lbl);
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
    if (g_graphics && is_opt(c)) {
        if (g_gfx_menu) {
            // The value virtual is called with (page, row): row 0 FXAA, row 1 MSAA.
            if (c->r[2] == ROW_FSR) c->r[0] = (uint32_t)fsr_sel();
            else if (c->r[2] == ROW_MSAA) c->r[0] = (uint32_t)msaa_slot(g_config.msaa);
            else c->r[0] = g_config.fxaa ? SLOT_ON : SLOT_OFF;
            return;
        }
        if (row_abs(c->r[0], c->r[1], c->r[2]) == GRAPHICS_ITEM) {
            c->r[0] = 0u;                     // the Graphics row shows no value
            return;
        }
    }
    fnhook_call_original(c, FN_OPT_VALUE);
}

static void opt_count(Cpu *c)
{
    if (g_graphics && is_opt(c)) {
        c->r[0] = g_gfx_menu ? GFX_ROWS : STOCK_ITEMS + 1u;
        return;
    }
    fnhook_call_original(c, FN_OPT_COUNT);
}

static void param_set(Cpu *c)
{
    if (g_gfx_menu) return;                   // the submenu rows must not apply the borrowed option
    fnhook_call_original(c, FN_PARAM_SET);
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
    g_refresh = 1;
    LOG("[pausepages] Graphics menu\n");
}

static void leave_menu(void)
{
    if (!g_gfx_menu) return;
    g_gfx_menu = 0;
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
    g_config.fsr = sel != FSR_OFF;
    g_config.fsr_upscaler = sel == FSR_4 ? FSR_UP_4 : FSR_UP_3;
    config_save_key("FSR", g_config.fsr ? "On" : "Off");
    config_save_key("Upscaler", sel == FSR_4 ? "FSR4" : "FSR3");
    if (!g_config.fsr) {
        g_fsr_on = false;
    } else if (g_fsr_boot && g_fsr_boot_up == g_config.fsr_upscaler) {
        g_fsr_on = true;                  // the upscaler built at start-up is used again
    } else {
        LOG("[pausepages] FSR %s takes effect on the next start (the upscaler is built at start-up)\n", fsr_text(sel));
    }
    LOG("[pausepages] FSR -> %s\n", fsr_text(sel));
}

void pausepages_input(uint64_t *buttons)
{
    if (g_swallow_b) {                        // the B that left the menu is still held
        if (*buttons & (1ull << BTN_B)) *buttons &= ~(1ull << BTN_B);
        else g_swallow_b = 0;
    }
    if (g_graphics && !g_gfx_menu && g_opt_obj) {
        // Options list. B/Plus leave it (let the engine close it) and the window handle is
        // dropped so the bar's own A presses are never mistaken for the extra row.
        if (g_b_pressed_ns != g_b_last) {
            g_b_last = g_b_pressed_ns;
            g_opt_obj = 0;
            return;
        }
        if (g_plus_pressed_ns != g_plus_last) {
            g_plus_last = g_plus_pressed_ns;
            g_opt_obj = 0;
            return;
        }
        // A on the Graphics row opens the menu instead of the stock action (that index runs
        // "Reset All Settings"), so the press never reaches the engine.
        if (g_a_pressed_ns != g_a_last) {
            g_a_last = g_a_pressed_ns;
            if (opt_flat(g_opt_obj) == GRAPHICS_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_menu();
            }
        }
        return;
    }
    if (!g_gfx_menu) return;
    // Safety net: if the window went away by some path other than B/Plus, stop touching input.
    static int hidden;
    if (g_opt_obj && !LD8(g_opt_obj + 0x18u)) {
        if (++hidden > 60) { hidden = 0; leave_menu(); return; }
    } else {
        hidden = 0;
    }
    *buttons &= ~(1ull << BTN_B);             // B belongs to this menu, not to the Options window
    if (g_b_pressed_ns != g_b_last) {
        g_b_last = g_b_pressed_ns;
        g_swallow_b = 1;
        leave_menu();
        return;
    }
    if (g_plus_pressed_ns != g_plus_last) {
        g_plus_last = g_plus_pressed_ns;
        leave_menu();
        return;
    }
    if (g_lr_pressed_ns != g_lr_last) {
        g_lr_last = g_lr_pressed_ns;
        int row = g_opt_obj ? (int)LD8(PW_ROW(g_opt_obj)) : ROW_FXAA;
        if (row == ROW_FSR) {
            int dir = (*buttons & (1ull << BTN_RIGHT)) ? 1 : (*buttons & (1ull << BTN_LEFT)) ? -1 : 0;
            if (dir) {
                int i = (fsr_sel() + (dir > 0 ? 1 : 2)) % 3;
                fsr_apply(i);
            }
        } else if (row == ROW_MSAA) {
            int dir = (*buttons & (1ull << BTN_RIGHT)) ? 1 : (*buttons & (1ull << BTN_LEFT)) ? -1 : 0;
            if (dir) {
                int i = (msaa_index(g_config.msaa) + (dir > 0 ? 1 : 3)) & 3;
                g_config.msaa = msaa_value(i);
                void vk_set_msaa(uint32_t);
                vk_set_msaa((uint32_t)g_config.msaa);
                config_save_key("MSAA", msaa_text(g_config.msaa));
                LOG("[pausepages] MSAA -> %s\n", msaa_text(g_config.msaa));
            }
        } else {
            g_config.fxaa = !g_config.fxaa;
            config_save_key("FXAA", g_config.fxaa ? "On" : "Off");
            LOG("[pausepages] FXAA -> %s\n", g_config.fxaa ? "On" : "Off");
        }
        g_lbl_cur ^= 1;                       // the label pointer must change for the row to redraw
        g_refresh = 1;
    }
}

void pausepages_init(void)
{
    const char *e = getenv("MHGU_PAUSE_PAGES");
    if (e && *e) g_pages = (uint32_t)strtoul(e, NULL, 10);
    const char *g = getenv("MHGU_PAUSE_GRAPHICS");
    if (g && *g) g_graphics = strcmp(g, "0") ? 1 : 0;
    if (!g_graphics && !g_pages) return;
    fnhook_set(FN_BAR_INIT, hle_register("pausepages:bar_init", bar_init));
    if (!g_graphics) return;
    fnhook_set(FN_OPT_COUNT, hle_register("pausepages:opt_count", opt_count));
    fnhook_set(FN_OPT_ROW, hle_register("pausepages:opt_row", opt_row));
    fnhook_set(FN_OPT_VALUE, hle_register("pausepages:opt_value", opt_value));
    fnhook_set(FN_OPT_UPDATE, hle_register("pausepages:opt_update", opt_update));
    fnhook_set(FN_PARAM_SET, hle_register("pausepages:param_set", param_set));
    LOG("[pausepages] Options: Graphics row + FXAA/MSAA menu\n");
}
