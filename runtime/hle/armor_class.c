// Armour appearance stays the piece the hunter equipped, while its stats follow the class of the equipped weapon.
//
// The game asks f_33c208 (manager, type, id) for an element of one of its per-type equipment tables (type 1..5 are the
// armour slots).  A series that is Blademaster-only or Gunner-only has a counterpart of the other class
// (armor_pairs.h, generated from armorSeriesData.asd).  We alias only that lookup: when the equipped weapon's class
// does not match the piece, the element returned is the counterpart's.  The equipment entry itself is never modified,
// so the id, name, model and transmog stay the player's, and nothing is written to the save.
//
// The alias is gated on the *caller* (see equip_data) because the same lookup also serves identity/name/model queries.
// research/armor-class.md section 3 documents what the gate actually covers in this build: the equipment-box and
// transmog/model path is aliased, while the stat recalculation resolves through f_33f03c and is not.  Run with
// MHGU_ARMOR_DEBUG=1 to log one "[armor] lookup" line per distinct caller and re-derive the gate after a rebuild.
//
// "Both classes" armour is not aliased.  Every class-split series in the data has a counterpart, so no defense
// scaling is needed for armour that can be equipped by the current class.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "hle/armor_class.h"
#include "hle/armor_pairs.h"
#include "rt.h"

#define FN_EQUIP_DATA  0x33c208u   // (manager, type, id) -> equipment data row
#define G_SPLAYER      0x1a85b58u  // sPlayer *: the local player
#define PLAYER_WEAPON  0x94u       // the weapon entry in sPlayer (type at +4)
#define PLAYER_ARMOR   0xc0u       // head; the other four are 0x2c apart (chest, arms, waist, legs)
#define ITEM_PTR       0x01936514u // pointer stored in every equipment entry

static bool is_gunner_type(uint16_t type) {
    return (type >= 11 && type <= 13) || type == 17;  // light/heavy bowgun, bow
}

static uint32_t g_last_weapon_raw;  // raw weapon +4 word, for the unrecognised-weapon debug log

static uint16_t current_weapon_type(void) {
    uint32_t sp = LD32(G_SPLAYER);
    if (sp < 0x08000000u || sp >= 0xa0000000u) return 0;
    uint32_t weapon = sp + PLAYER_WEAPON;
    if (LD32(weapon) != ITEM_PTR) return 0;
    uint16_t type = LD16(weapon + 4);
    if (type >= 1 && type <= 21) return type;
    // Outside the weapon categories: normally the sheathed-weapon placeholder (type 83).  weapon_class() keeps the
    // last known class across these, so this only logs under MHGU_ARMOR_DEBUG to catch any real unrecognised weapon.
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_ARMOR_DEBUG") != NULL;
    if (dbg && LD32(weapon + 4) != g_last_weapon_raw) {
        g_last_weapon_raw = LD32(weapon + 4);
        LOG("[armor] unknown weapon entry: +4=%08x (type=%u id=%u) +8=%08x +c=%08x\n",
            LD32(weapon + 4), LD16(weapon + 4), LD16(weapon + 6), LD32(weapon + 8), LD32(weapon + 0xc));
    }
    return 0;
}

// Blademaster/gunner class of the weapon the hunter is using.  +0x94 is the *drawn* item, which reads as a
// placeholder (type 83) while the weapon is sheathed (e.g. inside the item box), so current_weapon_type() returns 0
// at those moments.  Remember the class from the last real weapon and keep using it while the drawn entry is a
// placeholder, so the armour swap is not skipped just because the weapon is temporarily not in hand.
static uint8_t g_weapon_class;  // 0 unknown, 1 blademaster, 2 gunner

static uint8_t weapon_class(void) {
    uint16_t type = current_weapon_type();
    if (type) g_weapon_class = is_gunner_type(type) ? 2 : 1;
    return g_weapon_class;
}

// f_33c208(manager, type, id): for a class-incompatible armour row, ask the table for the counterpart instead.

// Debug census of the callers that reach this lookup for armour.  One line per distinct (caller, gate, decision) so
// the caller gate can be re-derived after any rebuild -- the 0x2d0000..0x2e3000 range below is build-specific, and
// an address appearing in this list is the evidence that the gate still fires there.  Bounded, so it cannot flood.
// Only active with MHGU_ARMOR_DEBUG=1.
static void log_lookup(uint32_t caller, uint32_t type, uint32_t id, uint32_t out, bool in_range) {
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_ARMOR_DEBUG") != NULL;
    if (!dbg) return;
    enum { MAX_SEEN = 64 };
    static uint32_t seen[MAX_SEEN];
    static int n_seen;
    bool swapped = out != id;
    uint32_t key = caller | (in_range ? 0x40000000u : 0u) | (swapped ? 0x80000000u : 0u);
    for (int i = 0; i < n_seen; i++)
        if (seen[i] == key) return;
    if (n_seen == MAX_SEEN) return;
    seen[n_seen++] = key;
    LOG("[armor] lookup caller=%08x type=%u id=%u in_range=%d swapped=%08x\n", caller, type, id, in_range,
        swapped ? out : 0);
}

static void equip_data(Cpu *c) {
    uint32_t type = c->r[1], id = c->r[2], out = id;
    bool armour = type >= 1 && type <= 5 && id < ARMOR_SERIES_COUNT;
    bool in_range = false;
    uint32_t caller = 0;
    if (armour) {
        uint32_t next = 0;
        caller = dispatch_function_at(c->r[14], &next);
        // The gate: only callers inside the equipment/skill calculation block are aliased.  The UI's identity, name
        // and model lookups live in other bands (0x3x/0x4x/0x7x/0x8x) and keep the real id.  Note that this covers
        // the direct call sites only: code that resolves a piece through f_33f03c never matches, because its call to
        // this function arrives with LR inside f_33f03c (see research/armor-class.md section 3).
        in_range = caller >= 0x2d0000u && caller < 0x2e3000u;
        if (in_range) {
            uint8_t cls = armor_series_class[id];
            uint8_t want = weapon_class();
            if (want && cls && cls != want) {
                uint16_t pair = armor_series_pair[id];
                if (pair != 0xffffu && pair < ARMOR_SERIES_COUNT && armor_series_class[pair] == want) out = pair;
            }
        }
        log_lookup(caller, type, id, out, in_range);
    }
    if (out != id) c->r[2] = out;
    fnhook_call_original(c, FN_EQUIP_DATA);
}

void armor_class_init(void) {
    fnhook_set(FN_EQUIP_DATA, hle_register("armorclass:equip_data", equip_data));
}

// The game removes class-incompatible armour from the equipped set when a weapon changes class.  Keep the pieces the
// player had: on a class change, put the saved entries back.  The entries themselves keep their original
// id/name/model and transmog; whether their contribution to the hunter's stats is aliased depends on the caller gate
// (see research/armor-class.md section 3).
typedef struct {
    uint32_t w[11];  // the whole 44-byte entry (pointer, type/id, transmog, the rest: box/equipped state)
    bool valid;
} SavedArmor;

static SavedArmor g_saved[5];
static uint32_t g_arm_link;  // box-slot link for the arms slot (sp+0x88), used by the equipment-box marker
static bool g_arm_link_valid;
static uint32_t g_last_weapon;  // for the MHGU_ARMOR_DEBUG weapon-type log

static uint32_t player_base(void) {
    uint32_t sp = LD32(G_SPLAYER);
    return sp >= 0x08000000u && sp < 0xa0000000u ? sp : 0;
}

static bool armor_entry(uint32_t sp, int i, uint32_t *entry) {
    uint32_t e = sp + PLAYER_ARMOR + (uint32_t)i * 0x2cu;
    if (LD32(e) != ITEM_PTR) return false;
    uint32_t w = LD32(e + 4);
    uint16_t type = (uint16_t)w, id = (uint16_t)(w >> 16);
    if (type != (uint16_t)(i + 1) || id == 0) return false;
    *entry = e;
    return true;
}

static void clear_auto_tm(uint32_t e, bool gunner) {
    uint32_t w = LD32(e + 4);
    uint16_t type = (uint16_t)w, id = (uint16_t)(w >> 16);
    if (type < 1 || type > 5 || id >= ARMOR_SERIES_COUNT) return;
    uint8_t cls = armor_series_class[id], want = gunner ? 2 : 1;
    if (!cls || cls == want) return;
    uint16_t pair = armor_series_pair[id];
    if (pair == 0xffffu) return;
    if (LD16(e + 8) == pair) ST16(e + 8, 0);
}

void armor_class_frame(void) {
    uint32_t sp = player_base();
    if (!sp) return;
    uint16_t weapon = current_weapon_type();
    uint8_t cls = weapon_class();  // cached: survives a sheathed/placeholder drawn entry
    bool gunner = cls == 2;
    bool arms_restored = false;

    static int dbg = -1;
    if (dbg < 0) dbg = getenv("MHGU_ARMOR_DEBUG") != NULL;
    if (dbg && weapon && weapon != (uint16_t)g_last_weapon) {
        g_last_weapon = weapon;
        LOG("[armor] weapon type=%u class=%s\n", weapon, is_gunner_type(weapon) ? "gunner" : "blademaster");
    }

    for (int i = 0; i < 5; i++) {
        uint32_t e;
        if (armor_entry(sp, i, &e)) {
            if (cls) clear_auto_tm(e, gunner);
            if (!g_saved[i].valid || LD32(e + 4) != g_saved[i].w[1]) {
                for (int k = 0; k < 11; k++) g_saved[i].w[k] = LD32(e + 4u * (uint32_t)k);
                g_saved[i].valid = true;
            }
            g_saved[i].w[2] = LD32(e + 8);
        } else if (g_saved[i].valid) {
            e = sp + PLAYER_ARMOR + (uint32_t)i * 0x2cu;
            for (int k = 0; k < 11; k++) ST32(e + 4u * (uint32_t)k, g_saved[i].w[k]);
            if (cls) clear_auto_tm(e, gunner);
            g_saved[i].w[2] = LD32(e + 8);
            if (i == 2) arms_restored = true;
        }
    }

    uint32_t arm_link = sp + 0x88u;
    if (arms_restored) {
        if (g_arm_link_valid) ST32(arm_link, g_arm_link);
    } else {
        uint32_t cur = LD32(arm_link);
        if (g_arm_link_valid && (g_arm_link >> 16) != 0xffffu && (cur >> 16) == 0xffffu)
            ST32(arm_link, g_arm_link);
        else
            g_arm_link = cur, g_arm_link_valid = true;
    }
}
