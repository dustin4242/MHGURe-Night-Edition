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
// Melody Effect window (verified by A-press poke run, shot pg_tabA.png). We retarget
// flat 14 to the Options submenu (case 10) as a placeholder; the dedicated Graphics
// window + synthesized "Graphics" label are a later phase (festa label quads need
// synthesizing; see docs/UI.md notes).
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define FN_BAR_INIT     0x877d0cu     // uUIQuestStartMenu init virtual (fills the paging struct)
#define FN_BAR_TABSW    0x877ee0u     // uUIQuestStartMenu tab-window switch virtual (+0x68)
#define FN_BAR_AKEY     0x86fc30u     // uUIQuestStartMenu +0x90 virtual: A-press tab handler
#define BAR_VPTR_RT     0x197aeb8u    // uUIQuestStartMenu object vptr (class table 0x177aeb0 + 8)
#define PAGE_IDX        0xe8u         // object words of the embedded paging struct at bar+0xd8
#define PAGE_CNT        0xeau
#define TAB_TOTAL       0xecu         // u16: total tabs over all pages
#define GRAPHICS_TAB    14u           // flat index of the extra third-page tab (page 2, row 0)

static uint32_t g_pages;              // forced page count (0: leave alone)
static int g_graphics;                // 1: claim the 14th tab and retarget it to Options

static void bar_init(Cpu *c) {
    uint32_t bar = c->r[0];
    fnhook_call_original(c, FN_BAR_INIT);
    if (bar && LD32(bar) == BAR_VPTR_RT) {
        if (g_graphics) ST16(bar + TAB_TOTAL, g_graphics == 1 ? 15 : g_graphics);
        if (g_pages && (LD32(bar + PAGE_IDX) >> 16) != g_pages) {
            ST16(bar + PAGE_CNT, g_pages);
            LOG("[pausepages] bar %08x: pages -> %u\n", bar, g_pages);
        }
    }
}

static void bar_a(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_graphics && bar && LD32(bar) == BAR_VPTR_RT && LD32(bar + 0x4cu) >= GRAPHICS_TAB) {
        ST32(bar + 0x4cu, 10);        // A opens the Options submenu under the Graphics row
        LOG("[pausepages] tab %08x: flat 14 -> Options window\n", bar);
    }
    fnhook_call_original(c, FN_BAR_AKEY);
}

static void bar_tabsw(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_graphics && bar && LD32(bar) == BAR_VPTR_RT && LD32(bar + 0x4cu) >= GRAPHICS_TAB) {
        ST32(bar + 0x4cu, 10);        // Options submenu window in place of the Melody default
        LOG("[pausepages] tab %08x: flat 14 -> Options window\n", bar);
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
    fnhook_set(FN_BAR_AKEY, hle_register("pausepages:bar_a", bar_a));
    fnhook_set(FN_BAR_TABSW, hle_register("pausepages:bar_tabsw", bar_tabsw));
    LOG("[pausepages] pause bar pages: %u%s\n", g_pages, g_graphics ? " + graphics tab" : "");
}
