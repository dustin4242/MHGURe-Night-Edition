// Weapon fusion catalog loader and policy (weapon-fusion.wfus, WFUS v1).
//
// A weapon entry maps (native category, id) to the material category of the
// monster whose weapon the player wants to wear. The members are the native
// item point values, so the shared material picker and plan validation work
// unchanged.
#include "weapon_fusion.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

enum { WEAPON_TYPE_LIMIT = 32, WEAPON_ID_LIMIT = 256, CATEGORY_LIMIT = 1024, MEMBER_LIMIT = 65536 };
typedef struct { uint16_t category; uint8_t points; } Weapon;
typedef struct { uint16_t offset, count; } Category;
static Weapon weapons[WEAPON_TYPE_LIMIT][WEAPON_ID_LIMIT];
static FusionMaterial members[MEMBER_LIMIT];
static Category categories[CATEGORY_LIMIT];
static unsigned member_count;
static bool ready;
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

bool weapon_catalog_ready(void) { return ready; }

static const Weapon *weapon_for(const FusionPiece *piece) {
    if (!ready || !piece || piece->slot >= WEAPON_TYPE_LIMIT || piece->armor_id >= WEAPON_ID_LIMIT) return NULL;
    const Weapon *weapon = &weapons[piece->slot][piece->armor_id];
    return weapon->category ? weapon : NULL;
}

bool weapon_fusion_classify(FusionPiece *piece) {
    const Weapon *weapon = weapon_for(piece);
    if (!weapon) return false;
    piece->rank = FUSION_RANK_UNKNOWN;  // no tier: any appearance fits any weapon
    return true;
}

bool weapon_fusion_recipe(FusionPiece *base, FusionPiece *appearance, FusionRecipe *recipe) {
    if (!base || !appearance || !recipe || base->slot != appearance->slot ||
        base->slot < 6 || base->slot >= WEAPON_TYPE_LIMIT) return false;
    const Weapon *weapon = weapon_for(appearance);
    if (!weapon) return false;
    const Category *category = categories + weapon->category;
    if (!category->count) return false;
    base->rank = appearance->rank = FUSION_RANK_UNKNOWN;
    *recipe = (FusionRecipe){appearance->armor_id, weapon->category, FUSION_RANK_UNKNOWN,
        members + category->offset, category->count};
    return true;
}

bool weapon_catalog_load(void) {
    ready = false; member_count = 0;
    memset(weapons, 0, sizeof weapons);
    memset(members, 0, sizeof members);
    memset(categories, 0, sizeof categories);
    char path[2048];
#ifdef __ANDROID__
    strcpy(path, "./mhgurecomp");
#else
    if (!plat_exe_path(path, sizeof path)) return false;
#endif
    char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash || (size_t)(slash - path) + 32 >= sizeof path) return false;
    strcpy(slash + 1, "weapon-fusion.wfus");
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char header[48] = {0}, row[8] = {0};
    bool ok = fread(header, 1, sizeof header, file) == sizeof header &&
        !memcmp(header, "WFUS", 4) && u32(header + 4) == 1;
    unsigned count = ok ? u32(header + 8) : 0;
    member_count = ok ? u32(header + 12) : 0;
    ok = ok && count <= WEAPON_TYPE_LIMIT * WEAPON_ID_LIMIT && member_count <= MEMBER_LIMIT;
    unsigned previous_type = 0, previous_id = 0;
    for (unsigned i = 0; ok && i < count; ++i) {
        ok = fread(row, 1, sizeof row, file) == sizeof row;
        unsigned type = row[0], id = u16(row + 2), category = u16(row + 4), points = row[6];
        ok = ok && !row[1] && type >= 6 && type < WEAPON_TYPE_LIMIT && id && id < WEAPON_ID_LIMIT &&
            category && category < CATEGORY_LIMIT && points == FUSION_POINTS && !row[7] &&
            (type > previous_type || (type == previous_type && id > previous_id));
        if (ok) weapons[type][id] = (Weapon){(uint16_t)category, (uint8_t)points};
        previous_type = type; previous_id = id;
    }
    previous_type = previous_id = 0;
    for (unsigned i = 0; ok && i < member_count; ++i) {
        ok = fread(row, 1, sizeof row, file) == sizeof row;
        unsigned category = u16(row), item = u16(row + 2), points = row[4];
        ok = ok && category && category < CATEGORY_LIMIT && item && points &&
            (category > previous_type || (category == previous_type && item > previous_id));
        if (ok) {
            Category *range = categories + category;
            if (!range->count) range->offset = (uint16_t)i;
            ++range->count;
            members[i] = (FusionMaterial){(uint16_t)item, (uint8_t)points};
        }
        previous_type = category; previous_id = item;
    }
    ok = ok && fgetc(file) == EOF;
    fclose(file);
    ready = ok;
    if (ok) fprintf(stderr, "[weapon-fusion] catalog: %u weapons, %u materials\n", count, member_count);
    return ok;
}
