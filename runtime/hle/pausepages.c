// MHGU_PAUSE_PAGES=<n> and MHGU_PAUSE_GRAPHICS=<0/1>: grow the in-hunt pause tab bar
// (uUIQuestStartMenu, class table 0x177aeb0, main_107/108.c) past its stock two pages.
//
// The page indicator and paging are live object data: the bar object embeds a festa
// cUIObjectSetupParam at +0xd8 (docs/UI.md: struct+0xd = row byte at +0xe5, +0xe rows
// per page at +0xe6, +0x10 = LE16 page index | LE16 page count at +0xe8, +0x12 count at
// +0xea, +0x14 total tabs u16 at +0xec; uUIQuestStartMenu::init f_877d0c fills it via
// f_87006c(bar, 3, 7, 1, 0xd) -> f_74edec + f_74ee34, count = ceil(13 tabs / 7 rows)).
// A hook on that init virtual raises the total-tab count and the page count so the bar
// pages a third time (page GUI is generic: rows are drawn from the paging struct, verified
// by poke in docs/UI.md, shots pc_2of3.png / pg_*.png).
//
// MHGU_PAUSE_GRAPHICS also claims a 14th tab on that third page: the tab-window switch
// virtual (+0x68 = f_877ee0, 13-way on the selector's flat index at object+0x4c) takes its
// default branch for flat >= 13, which shows the festa child at [bar+0x34][14] = the
// Melody Effect window (verified by A-press poke run, shot pg_tabA.png). Flat 14 is
// retargeted to the Options window (case 10) and that window is rebuilt as a dedicated
// one-row FXAA menu (docs/UI.md 5c); the tab's own label is still the Melody one.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../config.h"
#include "input.h"

#define FN_BAR_INIT     0x877d0cu     // uUIQuestStartMenu init virtual (fills the paging struct)
#define FN_PAGING_INIT  0x87006cu     // paging setup (bar,3,7,1,totalTabs) called by the init virtual
#define FN_BAR_TABSW    0x877ee0u     // uUIQuestStartMenu tab-window switch virtual (+0x68)
#define FN_BAR_AKEY     0x86fc30u     // uUIQuestStartMenu +0x90 virtual: A-press tab handler
#define BAR_VPTR_RT     0x197aeb8u    // uUIQuestStartMenu object vptr (class table 0x177aeb0 + 8)
#define PAGE_IDX        0xe8u         // object words of the embedded paging struct at bar+0xd8
#define PAGE_CNT        0xeau
#define TAB_TOTAL       0xecu         // u16: total tabs over all pages
#define GRAPHICS_TAB    14u           // flat index of the extra third-page tab (page 2, row 0)

static uint32_t g_pages;              // forced page count (0: leave alone)
static int g_gfx_menu;                // 1 while the Graphics submenu is the open window
static int g_retarget;                // bar_a is retargeting flat 14 -> 10 (its tab switch must not clear)
static uint64_t g_lr_last;             // last left/right press timestamp seen
static uint64_t g_b_last;              // last B press timestamp seen
static uint64_t g_plus_last;           // last Plus press timestamp seen
// The row text is baked into its nodes when the window lays out, and a node whose string
// pointer did not change is left alone. So the texts live in two host buffers and alternate:
// the pointer changes and the row is redrawn with the current state. The layout is then driven
// from opt_update. The buffers are laid out like the borrowed option slot ("label\0" then one
// value string per value, at the offsets the window uses for that item).
static uint32_t g_buf[2];
static int g_buf_cur;
static uint32_t g_lbl_src;            // the borrowed option string block (original bytes kept)
static uint32_t g_lbl_w[16];

static void enter_graphics(void);
static void leave_graphics(void);

static void restore_label(void)
{
    if (!g_lbl_src) return;
    for (int i = 0; i < 16; i++) ST32(g_lbl_src + (uint32_t)i * 4u, g_lbl_w[i]);
    g_lbl_src = 0;
}

static void fill_buf(void)
{
    if (!g_buf[0]) { g_buf[0] = hle_alloc(64); g_buf[1] = hle_alloc(64); }
    uint32_t b = g_buf[g_buf_cur];
    memset(G2H(b), 0, 64);
    ST32(b, 0x41415846u);                            // "FXAA"
    uint32_t v = g_config.fxaa ? 0x00006e4fu : 0x0066664fu;   // "On\0" / "Off\0"
    for (int i = 0; i < 5; i++) ST32(b + 13u + (uint32_t)i * 7u, v);
    if (g_lbl_src) {                                 // the value texts are looked up there
        ST32(g_lbl_src, 0x41415846u);
        ST8(g_lbl_src + 4u, 0u);
        for (int i = 0; i < 5; i++) ST32(g_lbl_src + 13u + (uint32_t)i * 7u, v);
    }
}
static int g_graphics;                // 1: claim the 14th tab and retarget it to Options

// f_87006c(bar, rows, _, totalTabs): the tab total is r3 (stock 13). It runs during the bar's
// init virtual, so raising it here makes the original init size the paging struct for a third
// page and build that page at once, instead of patching the paging struct afterwards.
static void paging_init(Cpu *c) {
    if (g_graphics && LD32(c->r[0]) == BAR_VPTR_RT) {
        uint32_t total = c->r[3];
        uint32_t want = (uint32_t)(g_graphics == 1 ? 15 : g_graphics);
        if (total != want) {
            c->r[3] = want;
            LOG("[pausepages] paging init bar %08x: tab total %u -> %u\n", c->r[0], total, want);
        }
    }
    fnhook_call_original(c, FN_PAGING_INIT);
}

static void bar_init(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_gfx_menu) {                     // a fresh pause menu always starts on the stock tabs
        leave_graphics();
    }
    fnhook_call_original(c, FN_BAR_INIT);
    if (bar && LD32(bar) == BAR_VPTR_RT) {
        if (g_graphics) ST16(bar + TAB_TOTAL, g_graphics == 1 ? 15 : g_graphics);
        if (g_pages && (LD32(bar + PAGE_IDX) >> 16) != g_pages) {
            ST16(bar + PAGE_CNT, g_pages);
            LOG("[pausepages] bar %08x: pages -> %u\n", bar, g_pages);
        }
    }
}

// Custom "Graphics" submenu: it is the real cUIOOptionWindowForGame, but opened on the third
// page it must show a single FXAA row. Its row count comes from the +0x60 virtual (f_7e7534,
// stock 0x1b = 27 items); reporting 1 makes the window lay out one row and one page.
#define FN_OPT_COUNT 0x7e7534u
#define OPT_VPTR_RT  0x1972c88u      // cUIOOptionWindowForGame object vptr

// Row plumbing for the one-row Graphics menu. The window's label virtual +0x68 is
// (out, page, item) and its value virtual +0x6c is (page, item) -> value. The single row is
// redirected to the Camera Angle slot (a long label with one value string per value), its
// texts come from our own buffers, and its value is the port's FXAA flag.
#define FN_OPT_ROW   0x7e7558u       // label/layout setup virtual (+0x68)
#define FN_OPT_VALUE 0x7e7d34u       // current-value virtual (+0x6c)
#define FN_PARAM_SET 0x85e5bcu       // festa parameter setter
#define LABEL_ITEM   4u             // "Camera Angle": a long label slot with separate value strings

// The borrowed slot is "Camera Angle\0Type 1\0..Type 5\0Camera Controls\0". The label comes
// from an alternating host buffer (a changed pointer is what makes the row redraw), and the
// value texts are written over the "Type N" strings in the option table, since the window
// looks those up by value index.
static uint32_t g_opt_obj;             // the Graphics menu window
static int g_refresh;                  // row text changed: rebuild the window's row nodes

#define FN_OPT_LAYOUT 0x7e635cu        // the Options window's row layout (main_102.c)

// The option row's text is baked into its nodes when the window lays out; the plain value
// change path would only rebuild it on the stock window. Drive the same layout ourselves.
static void opt_update(Cpu *c)
{
    fnhook_call_original(c, 0x7e5840u);
    if (g_refresh && g_gfx_menu && g_opt_obj) {
        g_refresh = 0;
        c->r[0] = g_opt_obj;
        GuestFn layout = dispatch_lookup(FN_OPT_LAYOUT);
        if (layout) layout(c);
    }
}

static void enter_graphics(void)
{
    g_gfx_menu = 1;
    g_lr_last = g_lr_pressed_ns;      // the press that opened the menu is not a toggle
    g_b_last = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
}

static void leave_graphics(void)
{
    g_gfx_menu = 0;
    g_refresh = 0;
    restore_label();
}

static void opt_row(Cpu *c) {
    int gfx = g_gfx_menu && LD32(c->r[0]) == OPT_VPTR_RT;
    if (gfx) {
        g_opt_obj = c->r[0];
        c->r[3] = LABEL_ITEM;             // the single drawn row borrows the "Camera Angle" slot
    }
    uint32_t out = c->r[1];
    fnhook_call_original(c, FN_OPT_ROW);
    if (gfx && out >= 0x0c000000u) {
        uint32_t src = LD32(out);
        if (!g_lbl_src && src >= 0x10000000u && src < 0x1c000000u) {
            g_lbl_src = src;
            for (int i = 0; i < 16; i++) g_lbl_w[i] = LD32(src + (uint32_t)i * 4u);
        }
        fill_buf();
        ST32(out, g_buf[g_buf_cur]);   // label from our buffer (alternates to force a redraw)
    }
}

static void opt_value(Cpu *c) {
    if (g_gfx_menu && LD32(c->r[0]) == OPT_VPTR_RT) {
        c->r[0] = g_config.fxaa ? 1u : 0u;
        return;
    }
    fnhook_call_original(c, FN_OPT_VALUE);
}

static void param_set(Cpu *c) {
    if (g_gfx_menu) return;          // the borrowed row must not apply any real option
    fnhook_call_original(c, FN_PARAM_SET);
}

// The window has one row, not an editable list: the input layer keeps A away from the engine
// (it would open the stock value editor on the borrowed row) and turns a left/right press into
// one FXAA flip. B and Plus still pass, so the menu and the pause menu close as usual.
void pausepages_filter_input(uint64_t *buttons)
{
    if (!g_gfx_menu) return;
    // Safety net: if the pause menu went away by some path other than B/Plus (the window is
    // hidden), stop masking input so the field controls keep working.
    static int hidden;
    if (g_opt_obj && !LD8(g_opt_obj + 0x18u)) {
        if (++hidden > 60) { hidden = 0; leave_graphics(); return; }
    } else {
        hidden = 0;
    }
    if (g_b_pressed_ns != g_b_last) {
        g_b_last = g_b_pressed_ns;
        leave_graphics();            // B backs out to the tab bar: hand the stock menu back
        return;
    }
    if (g_plus_pressed_ns != g_plus_last) {
        g_plus_last = g_plus_pressed_ns;
        leave_graphics();            // Plus closes the pause menu from anywhere
        return;
    }
    if (g_lr_pressed_ns != g_lr_last) {
        g_lr_last = g_lr_pressed_ns;
        g_config.fxaa = !g_config.fxaa;
        g_buf_cur ^= 1;
        fill_buf();
        g_refresh = 1;
        LOG("[pausepages] FXAA -> %s\n", g_config.fxaa ? "On" : "Off");
    }
    // A would open the stock value editor on the borrowed row: keep it away from the engine.
    // Left/right pass through so the window re-lays out the row (and re-reads the texts we
    // just rewrote); the value setter is swallowed so no real option is applied.
    *buttons &= ~(1ull << BTN_A);
}

static void opt_count(Cpu *c) {
    if (g_gfx_menu && LD32(c->r[0]) == OPT_VPTR_RT) {
        c->r[0] = 1;                // a single row, on its own page
        return;
    }
    fnhook_call_original(c, FN_OPT_COUNT);
}

static void bar_a(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_gfx_menu) return;           // the submenu owns its input; the bar must not act on it
    int retarget = g_graphics && bar && LD32(bar) == BAR_VPTR_RT && LD32(bar + 0x4cu) >= GRAPHICS_TAB;
    if (retarget) {
        ST32(bar + 0x4cu, 10);        // A opens the Options submenu under the Graphics row
        g_retarget = 1;               // the tab switch this A triggers is ours, not a tab change
        enter_graphics();
        LOG("[pausepages] tab %08x: flat 14 -> Graphics menu\n", bar);
    }
    fnhook_call_original(c, FN_BAR_AKEY);
    g_retarget = 0;
}

static void bar_tabsw(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_graphics && bar && LD32(bar) == BAR_VPTR_RT) {
        if (LD32(bar + 0x4cu) >= GRAPHICS_TAB) {
            ST32(bar + 0x4cu, 10);    // Options submenu window in place of the Melody default
            enter_graphics();
            LOG("[pausepages] tab %08x: flat 14 -> Graphics menu\n", bar);
        } else if (g_gfx_menu && !g_retarget && LD32(bar + 0x4cu) != 10u) {
            // flat 10 is our own retarget of the Graphics row; any other tab is the user
            // moving away, which must get the stock options window back.
            leave_graphics();
        }
    }
    fnhook_call_original(c, FN_BAR_TABSW);
}

void pausepages_init(void) {
    const char *e = getenv("MHGU_PAUSE_PAGES");
    if (e && *e) g_pages = (uint32_t)strtoul(e, NULL, 10);
    const char *g = getenv("MHGU_PAUSE_GRAPHICS");
    if (g && *g) g_graphics = strcmp(g, "0") ? 1 : 0;
    if (g_graphics && !g_pages) g_pages = 3;
    if (!g_pages) return;
    fnhook_set(FN_BAR_INIT, hle_register("pausepages:bar_init", bar_init));
    fnhook_set(FN_PAGING_INIT, hle_register("pausepages:paging_init", paging_init));
    fnhook_set(FN_OPT_COUNT, hle_register("pausepages:opt_count", opt_count));
    fnhook_set(FN_OPT_ROW, hle_register("pausepages:opt_row", opt_row));
    fnhook_set(FN_OPT_VALUE, hle_register("pausepages:opt_value", opt_value));
    fnhook_set(FN_PARAM_SET, hle_register("pausepages:param_set", param_set));
    fnhook_set(FN_BAR_AKEY, hle_register("pausepages:bar_a", bar_a));
    fnhook_set(FN_BAR_TABSW, hle_register("pausepages:bar_tabsw", bar_tabsw));
    fnhook_set(0x7e5840u, hle_register("pausepages:opt_update", opt_update));
    LOG("[pausepages] pause bar pages: %u%s\n", g_pages, g_graphics ? " + graphics tab" : "");
}
