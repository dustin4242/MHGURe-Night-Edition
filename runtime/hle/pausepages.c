// MHGU_PAUSE_PAGES=<n>: the in-hunt pause tab bar's page indicator (the "x / n" by the "Menu"
// title) shows <n> pages. The counter is live object data, not a rendered string: the bar
// object uUIQuestStartMenu keeps a word at object+0xe8 (main_107.c: LD16(+0xe8) page index,
// 0-based; LE16 at +0xea page count; the selector f_86f868 recomputes the flat tab index +0x4c
// = page*rows + row from it every update). Verified by poke in docs/UI.md (poke +0xe8 word
// 0x00020000 -> 0x00030001 draws the header as "2 / 3", shots pc_2of3.png).
// This mod hooks the bar's per-frame virtual (uUIQuestStartMenu +0x24 = f_86ed24, called with
// r0 = bar) and forces the count halfword, guarded by the object's class vptr so the shared
// thunk cannot touch foreign objects. NOTE: this makes the indicator read "x / n"; content for
// a page beyond the constructed two needs that page's GUI built in the ctor pattern
// (f_85ca84(bar, layout literal, page index) + its tab group f_870098/f_8700a4) — not done here.
#include <stdlib.h>

#include "../hle.h"

#define FN_BAR_UPDATE   0x86ed24u     // uUIQuestStartMenu virtual +0x24 (r0: the bar object)
#define BAR_VPTR_RT     0x197aeb8u    // uUIQuestStartMenu object vptr (class table 0x177aeb0 + 8)
#define PAGE_WORD       0xe8u         // object word: LE16 page index (0-based), LE16 page count

static uint32_t g_pages;              // the page count to force (0: off)

static void bar_update(Cpu *c) {
    uint32_t bar = c->r[0];
    if (g_pages && bar && LD32(bar) == BAR_VPTR_RT) {
        if ((LD32(bar + PAGE_WORD) >> 16) != g_pages) {
            ST16(bar + PAGE_WORD + 2, g_pages);
            LOG("[pausepages] bar %08x: page count -> %u\n", bar, g_pages);
        }
    }
    fnhook_call_original(c, FN_BAR_UPDATE);
}

void pausepages_init(void) {
    const char *e = getenv("MHGU_PAUSE_PAGES");
    if (!(e && *e) || !(g_pages = (uint32_t)strtoul(e, NULL, 10))) return;
    fnhook_set(FN_BAR_UPDATE, hle_register("pausepages:bar_update", bar_update));
    LOG("[pausepages] in-hunt pause bar page count: %u\n", g_pages);
}
