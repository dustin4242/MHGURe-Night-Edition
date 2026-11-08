# Weapon Fusion

The smithy's **Equipment Fusion** menu (the row added by [early armor
fusion](early-armor-fusion.md), now labelled that way) fuses weapons as well as
armor. The game's native fusion and separation screens are reused unchanged:
pick a base weapon, pick the appearance weapon, deliver materials and pay the
native zenny fee; separation undoes it.

Two things make weapons work:

- **No rank restriction.** The native fusion screen only lets a piece take an
  appearance from the same rank band (LR/LR, HR/HR, G/G). For weapons that gate
  is cleared: any LR, HR or G weapon can take any appearance weapon of the same
  weapon type (a Great Sword keeps a Great Sword model, and so on).
- **Materials by monster.** The fusion's material category is taken from the
  appearance weapon's own crafting recipe, so a Brachydios weapon asks for
  Brachydios parts and a Rathalos weapon for Rathalos parts. Ten points are
  needed, exactly like armor fusion; the game's own point picker, quantities,
  ownership limits and confirmation are used.

Fused weapons show the appearance weapon's model and icon in the box, the
equipment screens and in the field.

Hovering a weapon in the fusion box shows its real stats in the panel (attack,
sharpness, slots, affinity) and its model in the 3D preview, exactly like an
armor piece. The native fusion screen only knew armor, so the fork ships two
small code patches of its own (the same two the Weapon Transmog mod makes):
the equipment iteration visits types 1..21 instead of 1..5, and its validity
check accepts weapons. They are applied at start-up through the mods patch
queue (`mods_builtin_code_patch`, `armor_fusion_preview_patches`) and make the
two functions run in the interpreter; nothing else changes.

## Turning it off

Weapon fusion is part of the armor fusion feature and follows its switch:
`[Armor Fusion] Enabled = Off` in `config.ini` (next start) or
`MHGU_ARMOR_FUSION=0` in the environment removes the whole fusion row,
weapon fusion included. The catalogs must sit beside the executable:
`armor-fusion.afus` and `weapon-fusion.wfus` (both built by `make
fusion-catalog`). Without `weapon-fusion.wfus` the menu still opens but only
armor pieces can be chosen.

The weapon catalog is generated from the player's own dump; the test
`make test-armor-fusion` covers the fusion planner (including the weapon
rules) and the catalog reader.

## The menu text

The row was called "Armor Fusion" in the earlier feature. It now reads
**Equipment Fusion**, the native screen's two options read **Fuse Equipment**
and **Undo Equipment Fusion**, and the boxes say "Choose a Base Armor or
Weapon." / "Choose a Visual Armor or Weapon.".

The native screen's strings come from the game's own `Lb_LaboratoryMsg` tables
in `nativeNX/<lang>/arc/village/common_<lang>.arc`. `tools/fusion_text.py`
rewords the English ones in the build's `data/romfs` from the player's dump
(`make data`, tracked by `data/.fusion-text`): a GMD's message texts are the
NUL-separated strings at the end of the file, so replacing strings only changes
that pool and its size, and the archive is repacked into a new file. No other
mod's romfs is involved. Other languages keep the game's own wording.

(Android loads the player's own game files from the device, so it keeps the
stock wording for now; the row itself still says Equipment Fusion.)

## Materials

`tools/weapon_fusion_materials.py` builds `weapon-fusion.wfus` from
`weaponCreateW*.wcd` and the item tables. Each record is 38 bytes: page, weapon
id, the upgrade parent, then four materials `{item, quantity, flags}`. A
weapon's fusion category is the native category of its key material (the one
with flag 2), or of its most valuable categorised material; some weapons have
two recipes and both are candidates. 1029 of the 1058 weapons get a category
(140 categories); the ones whose recipes only name uncategorised items are left
out of the menu.

## Behavior details

- The fusion box lists the whole equipment box; weapons are selectable because
  the adapter returns the native "valid" mask 1 for catalogued weapons (the
  native mask has extra bits that the list treats as "not selectable").
- The native pair check still runs; the adapter only relaxes it for weapons
  (same weapon type, different ids, not the same box slot).
- The material question and the point picker are the armor feature's; the
  question says "weapons" or "armor" depending on the pieces.
- Everything is a transaction like armor fusion: the fee is quoted from the
  game's own confirmation, materials are consumed only on approval, and
  cancelling spends nothing.
