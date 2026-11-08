// Weapon fusion catalog: appearance weapon -> native material category.
// The adapter and the confirmation plan live in hle/armorfusion.c; this is
// read-only policy, like armor_fusion.h.
#ifndef WEAPON_FUSION_H
#define WEAPON_FUSION_H
#include "armor_fusion.h"

// Loads weapon-fusion.wfus beside the executable. Failure leaves weapons out
// of the fusion menu; armour fusion keeps working.
bool weapon_catalog_load(void);
bool weapon_catalog_ready(void);

// A weapon piece is one whose native category has a recipe (types 7..21 except
// the native gap); no rank is assigned, because weapons fuse across ranks.
bool weapon_fusion_classify(FusionPiece *piece);
bool weapon_fusion_recipe(FusionPiece *base, FusionPiece *appearance, FusionRecipe *recipe);

// Same contract as fusion_plan, with weapon pairing rules: equal weapon type,
// different ids, no rank restriction and only the native "weapon" bit allowed
// in the pair result.
bool weapon_fusion_plan(const FusionPiece *base, const FusionPiece *appearance,
    uint32_t native_pair_result, const FusionRecipe *recipe,
    const FusionSelection *selection, size_t count,
    FusionOwned owned, void *context, FusionPlan *out);
#endif
