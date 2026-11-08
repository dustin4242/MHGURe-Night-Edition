// Weapon fusion: the game stores an appearance id for every equipment entry (+8: the armour fusion's transmog field)
// but its weapon draw path looked the *real* id up, so a fused-looking weapon drew its own model. The layered-weapons
// patch redirects that one call to the appearance-aware lookup (f_33f060 instead of f_33f03c); here the same thing is
// done as a hook on the lookup, gated on the weapon draw path's return address. Armour already went through
// f_33f060, which is why armour transmog always worked.
//
// The fusion UI itself (row, selection, materials) is runtime/hle/armorfusion.c; this file only makes the appearance
// win when the model is drawn.
#include <stdint.h>
#include <stdlib.h>

#include "../hle.h"
#include "../rt.h"

#define FN_EQUIP_LOOKUP   0x33f03cu  // (context, entry) -> equipment data row for the entry's real id
#define FN_ROW_LOOKUP     0x33c208u  // (manager, type, id) -> row
#define WEAPON_DRAW_LR    0x4874f0u  // the weapon draw path's return address (0x4874ec's call)

static int enabled = -1;

static bool on(void) {
    if (enabled < 0) {
        const char *e = getenv("MHGU_WEAPON_FUSION");
        enabled = e && *e ? atoi(e) != 0 : 1;
    }
    return enabled != 0;
}

static void equip_lookup(Cpu *c) {
    // Only the weapon draw path's call: every other caller keeps the real id (the box, the stats panel, the name).
    if (on() && c->r[14] == WEAPON_DRAW_LR && c->r[1]) {
        uint32_t entry = c->r[1];
        unsigned type = LD16(entry + 4) & 0x1fu;
        unsigned id = LD16(entry + 8);
        if (!id) id = LD16(entry + 6);
        static int trace = -1, left;
        if (trace < 0) trace = getenv("MHGU_FUSION_TRACE") != NULL, left = 200;
        if (trace && left > 0 && (left-- % 8) == 0)
            LOG("[weapon-fusion] draw lookup entry %x type %u appearance %04x (real %04x) row %s\n", entry, type,
                LD16(entry + 8), LD16(entry + 6), type >= 1 && type <= 0x18 ? "lookup" : "none");
        if (type >= 1 && type <= 0x18) {
            c->r[0] = guest_call4(c, FN_ROW_LOOKUP, c->r[0], type, id, 0);
            return;
        }
        c->r[0] = 0;
        return;
    }
    fnhook_call_original(c, FN_EQUIP_LOOKUP);
}

void weaponfusion_init(void) {
    static const struct { uint32_t addr, word; } anchors[] = {
        {FN_EQUIP_LOOKUP, 0xe1d120b4},  // ldrh r2, [r1, #4]
        {FN_ROW_LOOKUP, 0xe92d4070},    // push {r4, r5, r6, lr}
    };
    for (size_t i = 0; i < sizeof anchors / sizeof anchors[0]; ++i) {
        if (LD32(anchors[i].addr) != anchors[i].word) {
            LOG("[weapon-fusion] unsupported guest function %x; off\n", anchors[i].addr);
            return;
        }
    }
    if (!fnhook_set(FN_EQUIP_LOOKUP, hle_register("weaponfusion:lookup", equip_lookup)))
        LOG("[weapon-fusion] the equipment lookup is not a recompiled function: off\n");
}
