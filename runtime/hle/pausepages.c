// MHGU_PAUSE_PAGES=<n>: grow the in-hunt pause tab bar (uUIQuestStartMenu, class table
// 0x177aeb0, main_107/108.c) past its stock two pages (docs/UI.md 5b).
//
// MHGU_PAUSE_GRAPHICS=<0/1>: add a "Graphics" entry as the last row of the Options menu
// (cUIOOptionWindowForGame) and open a one-row FXAA On/Off menu from it. The stock options
// keep their own rows and indices, so nothing about their values changes.
//
// The window reports its item count through virtual +0x60 (f_7e7534, stock 27); reporting 28
// gives the list one extra row on its last page. That row is drawn from the HUD slot (item 2,
// a plain On/Off row) with the label "Graphics" and an empty value. Selecting it opens the
// menu editor on the extra item; the window update sees that and switches this same window to
// the one-row FXAA view. Backing out restores the Options list at the Graphics row.
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
// (obj+0x5a), page u16 +0x10 (obj+0x5c), page count u16 +0x12 (obj+0x5e), flat u16 +0x14
// (obj+0x60). The window's edit state is +0x74 (2 while a value editor is open).
#define PW_ROW(w)   ((w) + 0x59u)
#define PW_ROWS(w)  ((w) + 0x5au)
#define PW_PAGE(w)  ((w) + 0x5cu)

#define STOCK_ITEMS 27u               // rows of the stock Options list
#define GRAPHICS_ITEM STOCK_ITEMS     // the extra row (last)
#define FXAA_ITEM   4u                // "Camera Angle": a long label slot with one string per value
#define GFX_ITEM    2u                // "HUD": a plain On/Off slot (draws the Graphics row)

static uint32_t g_pages;              // forced bar page count (MHGU_PAUSE_PAGES; 0: leave alone)
static int g_graphics;                // 1: Graphics row / FXAA menu enabled
static int g_fxaa_menu;               // 1: the one-row FXAA menu is the current view
static uint32_t g_opt_obj;            // the Options window
static int g_refresh;                 // rebuild the row nodes on the next update

static uint64_t g_a_last;             // last A press timestamp seen
static uint64_t g_lr_last;            // last left/right press timestamp seen
static uint64_t g_b_last;             // last B press timestamp seen
static uint64_t g_plus_last;          // last Plus press timestamp seen
static int g_swallow_b;               // keep B from the engine until it is released

// FXAA row texts. The row label alternates between two buffers: the window only redraws a text
// node whose string pointer changed. The value strings are looked up in the option table, so the
// "Type N" strings of the borrowed slot are rewritten while the FXAA row shows.
static uint32_t g_lbl[2];
static int g_lbl_cur;
static uint32_t g_tbl;                // borrowed slot (Camera Angle), patched while the FXAA row shows
static uint32_t g_tbl_w[16];

// Graphics row label, and the HUD value string it blanks for its own row only.
static uint32_t g_gfx_lbl;
static uint32_t g_blank_addr;
static uint8_t g_blank_byte;
static int g_blank;

// The Options pager is shrunk to one row while the FXAA menu shows; remember its shape.
static int g_saved_rows;
static uint32_t g_saved_pages;
static uint32_t g_saved_total;
static uint32_t g_saved_page, g_saved_row;

static void enter_fxaa(void);
static void leave_fxaa(void);

static void ensure_bufs(void)
{
    if (g_lbl[0]) return;
    g_lbl[0] = hle_alloc(64);
    g_lbl[1] = hle_alloc(64);
    g_gfx_lbl = hle_alloc(16);
    ST32(g_gfx_lbl, 0x70617247u);                // "Grap"
    ST32(g_gfx_lbl + 4u, 0x73636968u);           // "hics"
    ST8(g_gfx_lbl + 8u, 0u);
}

static void restore_tbl(void)
{
    if (!g_tbl) return;
    for (int i = 0; i < 16; i++) ST32(g_tbl + (uint32_t)i * 4u, g_tbl_w[i]);
    g_tbl = 0;
}

static void fill_lbl(void)
{
    ensure_bufs();
    uint32_t b = g_lbl[g_lbl_cur];
    memset(G2H(b), 0, 64);
    ST32(b, 0x41415846u);                        // "FXAA"
    ST8(b + 4u, 0u);
    uint32_t v = g_config.fxaa ? 0x00006e4fu : 0x0066664fu;   // "On\0" / "Off\0"
    for (int i = 0; i < 5; i++) ST32(b + 13u + (uint32_t)i * 7u, v);
    if (g_tbl) {                                 // the value texts are looked up there
        ST32(g_tbl, 0x41415846u);
        ST8(g_tbl + 4u, 0u);
        for (int i = 0; i < 5; i++) ST32(g_tbl + 13u + (uint32_t)i * 7u, v);
    }
}

static void bar_init(Cpu *c) {
    if (g_fxaa_menu) leave_fxaa();
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

// The input virtual rebuilds the row text before reading the pager; not needed here, but the
// layout may be driven from opt_update after a text change.
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
    int gfx_row = 0;
    if (on) {
        if (g_opt_obj != c->r[0]) {           // the window just opened: its opening press is not input
            g_opt_obj = c->r[0];
            g_a_last = g_a_pressed_ns;
            g_b_last = g_b_pressed_ns;
            g_plus_last = g_plus_pressed_ns;
        }
        if (g_fxaa_menu) {
            c->r[3] = FXAA_ITEM;
        } else if (abs == GRAPHICS_ITEM) {
            c->r[3] = GFX_ITEM;              // draw the Graphics row from the HUD slot
            gfx_row = 1;
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
    if (g_fxaa_menu) {
        if (!g_tbl && src >= 0x10000000u && src < 0x1c000000u) {
            g_tbl = src;
            for (int i = 0; i < 16; i++) g_tbl_w[i] = LD32(src + (uint32_t)i * 4u);
        }
        fill_lbl();
        ST32(out, g_lbl[g_lbl_cur]);
    } else if (gfx_row) {
        ensure_bufs();
        ST32(out, g_gfx_lbl);                 // label "Graphics" (always, whatever the stock row was)
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
        if (g_fxaa_menu) {
            c->r[0] = g_config.fxaa ? 1u : 0u;
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
        c->r[0] = g_fxaa_menu ? 1u : STOCK_ITEMS + 1u;
        return;
    }
    fnhook_call_original(c, FN_OPT_COUNT);
}

static void param_set(Cpu *c)
{
    if (g_fxaa_menu) return;                  // the FXAA row must not apply the borrowed option
    fnhook_call_original(c, FN_PARAM_SET);
}

static void enter_fxaa(void)
{
    if (g_fxaa_menu) return;
    g_fxaa_menu = 1;
    uint32_t w = g_opt_obj;
    if (w) {
        g_saved_rows = LD8(PW_ROWS(w));
        g_saved_pages = LD16(w + 0x5eu);
        g_saved_total = LD16(w + 0x60u);      // +0x14 of the paging struct: total items
        g_saved_page = LD16(PW_PAGE(w));
        g_saved_row = LD8(PW_ROW(w));
        ST8(PW_ROW(w), 0u);
        ST16(PW_PAGE(w), 0u);
        ST8(PW_ROWS(w), 1u);
        ST16(w + 0x5eu, 1u);
        ST16(w + 0x60u, 1u);
        ST32(w + 0x74u, 0u);              // leave the borrowed slot's edit state
    }
    g_lr_last = g_lr_pressed_ns;          // the presses so far are not input for this view
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_refresh = 1;
    LOG("[pausepages] Graphics -> FXAA menu\n");
}

static void leave_fxaa(void)
{
    if (!g_fxaa_menu) return;
    g_fxaa_menu = 0;
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
    g_a_last = g_a_pressed_ns;            // presses made inside the menu are not list input
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_lr_last = g_lr_pressed_ns;
    g_refresh = 1;
    LOG("[pausepages] FXAA menu -> Options list\n");
}

void pausepages_input(uint64_t *buttons)
{
    if (g_swallow_b) {                    // the B that left the FXAA menu is still held
        if (*buttons & (1ull << BTN_B)) *buttons &= ~(1ull << BTN_B);
        else g_swallow_b = 0;
    }
    if (g_graphics && !g_fxaa_menu && g_opt_obj) {
        // Options list. B/Plus leave it (let the engine close it) and the window handle is
        // dropped so the bar's own A presses are never mistaken for the Graphics row.
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
        // A on the Graphics row opens our menu instead of the stock action (that index runs
        // "Reset All Settings"), so the press never reaches the engine.
        if (g_a_pressed_ns != g_a_last) {
            g_a_last = g_a_pressed_ns;
            if (opt_flat(g_opt_obj) == GRAPHICS_ITEM) {
                *buttons &= ~(1ull << BTN_A);
                enter_fxaa();
            }
        }
        return;
    }
    if (!g_fxaa_menu) return;
    // Safety net: if the window went away by some path other than B/Plus, stop touching input.
    static int hidden;
    if (g_opt_obj && !LD8(g_opt_obj + 0x18u)) {
        if (++hidden > 60) { hidden = 0; leave_fxaa(); return; }
    } else {
        hidden = 0;
    }
    *buttons &= ~(1ull << BTN_B);           // B belongs to this menu, not to the Options window
    if (g_b_pressed_ns != g_b_last) {
        g_b_last = g_b_pressed_ns;
        g_swallow_b = 1;
        leave_fxaa();
        return;
    }
    if (g_plus_pressed_ns != g_plus_last) {
        g_plus_last = g_plus_pressed_ns;
        leave_fxaa();
        return;
    }
    if (g_lr_pressed_ns != g_lr_last) {
        g_lr_last = g_lr_pressed_ns;
        g_config.fxaa = !g_config.fxaa;
        g_lbl_cur ^= 1;                   // the label pointer must change for the row to redraw
        g_refresh = 1;
        LOG("[pausepages] FXAA -> %s\n", g_config.fxaa ? "On" : "Off");
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
    LOG("[pausepages] Options: Graphics row (last) + FXAA menu\n");
}
