// Armour appearance stays the piece the hunter equipped, while its stats follow the class of the equipped weapon.
//
// The game asks f_33c208 (type, id) for an equipment data row.  Armour rows are types 1..5; a row whose series is
// Blademaster-only or Gunner-only has a counterpart of the other class (armor_pairs.h, generated from
// armorSeriesData.asd).  We alias only that lookup: when the equipped weapon's class does not match the piece, the
// data row returned is the counterpart's.  The equipment itself is never modified, so:
//   - the model, name, colour and existing transmog stay exactly as the player set them (no transmog flag added),
//   - the class restriction and its UI use the counterpart's class data,
//   - nothing is written to the save,
//   - changing the weapon changes the alias immediately; the game's normal equipment-change recalculation reads the
//     new rows.
//
// "Both classes" armour is not aliased.  Every class-split series in the data has a counterpart, so no defense
// scaling is needed for armour that can be equipped by the current class.

#include <stdbool.h>
#include <stdint.h>

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

static uint16_t current_weapon_type(void) {
    uint32_t sp = LD32(G_SPLAYER);
    if (sp < 0x08000000u || sp >= 0xa0000000u) return 0;
    uint32_t weapon = sp + PLAYER_WEAPON;
    if (LD32(weapon) != ITEM_PTR) return 0;
    uint16_t type = LD16(weapon + 4);
    return type >= 1 && type <= 21 ? type : 0;
}

// f_33c208(manager, type, id): for a class-incompatible armour row, ask the table for the counterpart instead.
static void equip_data(Cpu *c) {
    uint32_t type = c->r[1], id = c->r[2];
    if (type >= 1 && type <= 5 && id < ARMOR_SERIES_COUNT) {
        uint32_t next = 0, caller = dispatch_function_at(c->r[14], &next);
        // Only the equipment-stat/skill calculation code in main_012 uses the counterpart row.  The UI's identity,
        // name and model lookups go through their own callers and keep the real id.
        if (caller >= 0x2d0000u && caller < 0x2e3000u) {
            uint8_t cls = armor_series_class[id];
            uint16_t weapon = current_weapon_type();
            uint8_t want = is_gunner_type(weapon) ? 2 : 1;
            if (weapon && cls && cls != want) {
                uint16_t pair = armor_series_pair[id];
                if (pair != 0xffffu && pair < ARMOR_SERIES_COUNT && armor_series_class[pair] == want) c->r[2] = pair;
            }
        }
    }
    fnhook_call_original(c, FN_EQUIP_DATA);
}

void armor_class_init(void) {
    fnhook_set(FN_EQUIP_DATA, hle_register("armorclass:equip_data", equip_data));
}

// The game removes class-incompatible armour from the equipped set when a weapon changes class.  Keep the pieces the
// player had: on a class change, put the saved entries back.  The stat hook still makes their data follow the new
// class, and the entries themselves keep their original id/name/model and transmog.
typedef struct {
    uint32_t w[11];  // the whole 44-byte entry (pointer, type/id, transmog, the rest: box/equipped state)
    bool valid;
} SavedArmor;

static SavedArmor g_saved[5];
static uint32_t g_arm_link;  // box-slot link for the arms slot (sp+0x88), used by the equipment-box marker
static bool g_arm_link_valid;

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
    bool arms_restored = false;

    for (int i = 0; i < 5; i++) {
        uint32_t e;
        if (armor_entry(sp, i, &e)) {
            if (weapon) clear_auto_tm(e, is_gunner_type(weapon));
            if (!g_saved[i].valid || LD32(e + 4) != g_saved[i].w[1]) {
                for (int k = 0; k < 11; k++) g_saved[i].w[k] = LD32(e + 4u * (uint32_t)k);
                g_saved[i].valid = true;
            }
            g_saved[i].w[2] = LD32(e + 8);
        } else if (g_saved[i].valid) {
            e = sp + PLAYER_ARMOR + (uint32_t)i * 0x2cu;
            for (int k = 0; k < 11; k++) ST32(e + 4u * (uint32_t)k, g_saved[i].w[k]);
            if (weapon) clear_auto_tm(e, is_gunner_type(weapon));
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
