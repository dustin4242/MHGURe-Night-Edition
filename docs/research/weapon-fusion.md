# Weapon Fusion — reverse-engineering notes

How the *Weapon Transmog (Layered Weapons)* mod works, what it changes in MHGU 1.4.0, and how the same effects fit
this fork (whose early Armor Fusion moved the native fusion UI to every forge through `runtime/hle/armorfusion.c`).

> **Status, 2026-11-08 (done):** weapon fusion works in the smithy's Equipment Fusion menu. Verified live with the
> real save: a Great Sword takes another Great Sword's appearance, the native material picker opens for the
> appearance weapon's monster (**Brachydios**, category 173, derived from the weapon's crafting recipe) and the
> points can be added; the user confirmed the fusion end to end. The menu row reads "Equipment Fusion",
> the native rows read "Fuse Equipment" and "Undo Equipment Fusion". The drawing change
> (`runtime/hle/weaponfusion.c`) installs. Details in section 8.

## 1. What the mod ships

`addons/weapontransmog/Weapon Transmog (Layered Weapons)/`

| File | Role |
|------|------|
| `exefs/FB08F1D20FD1204FE7070672777F869B.ips` | 38 records, 37 code words + 1 data byte, into the main NSO |
| `romfs/nativeNX/<lang>/arc/village/common_<lang>.arc` | the fusion menu's texts, only so they say "weapons" too |

The README states the design: *"The game already stored an appearance field for weapons but ignored it when drawing
them."* The IPS makes the draw path read that field, and widens the native fusion UI from armour to weapons (same
weapon type only), 0z and no materials.

## 2. Address mapping

- IPS offsets are **file** offsets into `exefs/main`; the NSO text segment starts at file `0x100`.
- The port recompiles `main` with base **`0x00200000`**, so:

  `guest = file - 0x100 + 0x200000`  and  `file = guest - 0x1ffff00`

  (the mod's `0x2875ec` → guest `0x4874ec`; our generated `f_4874ec`).
- `0x33f03c`, `0x33f060`, `0x2db09c`, `0x33c208` below are guest addresses (image + 0x200000).

## 3. The equipment entry's appearance fields

Equipment entry (44 bytes), from `research/armor-class.md` and the mod:

| Offset | Content |
|--------|---------|
| `+0x04` | bits 0..4 type (1..5 armour, 6..21 weapons), bits 5..9 level, **bits 10..14 appearance level** (weapons) |
| `+0x06` | id (u16) |
| `+0x08` | **appearance id (u16)**, 0 = none; armour fusion has always written it |

- `f_2db09c(entry)` returns `LD16(entry+8) ? LD16(entry+8) : LD16(entry+6)` — the appearance id.
- `f_33f03c(context, entry)` looks the equipment row up by the **real** id (`+6`).
- `f_33f060(context, entry)` does the same through `f_2db09c`, i.e. by the **appearance** id, and also validates the
  type (1..0x18).

## 4. What the 38 records do

Old → new (guest address; file offset = guest − 0x1ffff00):

| Guest | Function | Change | Meaning |
|-------|----------|--------|---------|
| `0x4874ec` | `f_487454` weapon draw path | `bl f_33f03c` → `bl f_33f060` | **the core**: the drawn model resolves through the appearance id. Armour already used `f_33f060` (its branch is at `0x487484`); only the weapon branch used the real id. |
| `0x340b9c` | `f_340b7c` = **`FN_VALID`** (the fusion UI's per-item validity, also hooked by `armorfusion.c`) | `mov r0,r5; mov r4,#0x1001; bl 0x20daee8` → `and r0,r0,#0x1f; mov r4,#0x1001; cmp r0,#0x16; orr r4,r4,#2` | accept weapon types (up to 22) in the box list instead of asking "is this armour?" |
| `0x340ba4`, `0x340ba8` | `f_340b7c` | continuation of the same replacement | (with `0x340b9c`) |
| `0x340c0c` | `f_340b7c` | `b 0x340c7c` → `b 0x340ca4` | skip the armour-slot check for weapons |
| `0x349120` | `f_348f00` (fusion box list/update) | new `if (type >= 7) goto 0x349150` before the armour path | weapons take the weapon descriptor path |
| `0x349154`, `0x349158`, `0x34915c` | `f_348f00` | `orr r0,r0,#0x7c00; strh r0,[r4,#4]` | mark the appearance-level field (bits 10..14) on the box row |
| `0x349290` | `f_3491e4` (box row descriptor) | reads `+4`/`+8`, moves bits 10..14 into the level field, packs the appearance id | the fusion rows carry the appearance level for weapons |
| `0x3492b4..0x3492bc` | `f_3491e4` | same block | ditto |
| `0x34a3d8` | `f_349e6c` (validity) | `bl 0x20daeb8` → `ldrb r0,[r0,#4]; and #31; cmp #6; …` | "is armour" becomes "is armour or weapon" |
| `0x863a80` | `f_863960` | `mov r1,#5` → `mov r1,#0x15` (21) | equipment type range 1..21 instead of 1..5 (box/equipment iteration) |
| `0x8646f0`, `0x86470f` | `f_8645cc` | `bl 0x20daeb8` → `mov r0,#1` | force "valid" in that check |
| `0x6ad94c` (data) | literal pool | `0x64` → `0x48` (100 → 72) | one data word; purpose not identified (neither 100 nor 72 appears as a table count nearby) |

Everything else in the UI is the game's own native fusion flow: it writes the appearance id/level into the base
weapon's entry (`+8`, bits 10..14 of `+4`) exactly as it does for armour.

## 5. Why it breaks this fork

Our early Armor Fusion is not the native Wycademy unlock: `armorfusion.c` adds the row to every forge, opens the same
native fusion screens and **hooks** `FN_VALID` (`0x340b7c`), `FN_PAIR`, `FN_QUOTE`, `FN_CONFIRM`, `FN_CONSUME`, …

- The mod patches **inside `0x340b7c`**, the very function our adapter hooks: the hook runs the original first, so a
  guest-code patch there survives, but our adapter then applies its own armour-only clearing (`bits 0x18`) and its
  `pieces()`/`snapshot()` require slots 1..5, ranks equal and an armour recipe — so a weapon selection is rejected at
  confirmation.
- Our adapter also forces the native fusion menu to two rows and replaces the material/confirm flow, so the mod's
  type widening is not enough on its own.

## 6. Fitting it into this build

Because the game is recompiled, IPS words cannot be applied to guest memory (they would only affect the interpreter);
each instruction has to become either a `tools/gen.py` PATCH or a runtime `fnhook_set` hook. Plan:

1. **Drawing (done as a hook):** `runtime/hle/weaponfusion.c` hooks `f_33f03c`; when the return address is
   `0x4874f0` (the weapon draw path) it performs the appearance-aware lookup (`type = entry[+4]&0x1f`,
   `id = entry[+8] ?: entry[+6]`, `f_33c208`). Verified: the hook installs and logs the draw path's entry
   (`type 8, id 1` in the character-select/town runs). `MHGU_WEAPON_FUSION=0` turns it off.
2. **UI (to do):** extend `armorfusion.c` to weapons:
   - accept types 6..21 in `validity()` (call the original, keep `0x18` only for armour),
   - `pieces()`: slots 1..21, appearance must be the same weapon type,
   - **no rank restriction** (the user's requirement): skip the LR/HR/G tier equality for weapons,
   - recipe: materials from the *appearance weapon's* monster (a new weapon catalog, `tools/weapon_materials.py`
     deriving monster/category/points from `weaponCreateW03.wcd` and the item tables, like `fusion_materials.py`),
   - the fee: native or a flat amount,
   - separation: the native "Undo Equipment Fusion" already clears `+8`.
3. **The remaining IPS words** (`0x340b9c..`, `0x349120..`, `0x863a80`, `0x8646f0..`) only teach the box/validity
   screens to list weapons. With our adapter opening the screens itself, prefer doing that in the adapter (validity
   hook + box filter) rather than patching those functions; only if the native list still hides weapons do they need
   `gen.py` PATCHES entries.
4. **Config:** `[Weapon Fusion] Enabled` beside `[Armor Fusion] Enabled` (`runtime/config.c` `ON_OFF`), default on,
   `MHGU_WEAPON_FUSION=0` override.

## 7. Verification

- Host: `make` + `make test-armor-fusion` stay green.
- In-game: the smithy (teleport `1160.35 -699.57 -49.19` yaw `145.5`), fuse a weapon with an appearance of another
  weapon of the same type and a different rank; the drawn model changes while the name/stats/box icon stay the base
  weapon's; saving and reloading keeps it; "Undo" restores it.
- The real save is backed up before the first transaction (`~/.local/share/MHGURecomp/save/save_0`).

## 8. Implementation in this fork (2026-11-08)

Files added (all read-only policy or hooks; the game still mutates equipment):

| File | Role |
|------|------|
| `tools/weapon_fusion_materials.py` | builds `weapon-fusion.wfus` from `weaponCreateW*.wcd`, `itemData.itm`, `itemCategoryTypeData.ict` |
| `runtime/weapon_fusion.[ch]` | WFUS v1 loader, weapon classification and recipe (no rank restriction) |
| `runtime/hle/weaponfusion.c` | the drawing hook (appearance-id equipment lookup on the weapon draw path) |
| `runtime/armor_fusion.c` | `plan()` generalized: weapons skip tier equality and only the native "weapon" bit may be in the pair result |
| `runtime/hle/armorfusion.c` | weapon pieces in `pieces()`, `validity()`, `pair()`, `snapshot()`, the material question and the plan dispatch |
| `Makefile` | `weapon-fusion.wfus` rule; `test-armor-fusion` links `runtime/weapon_fusion.c` |

### Material mapping

`weaponCreateWxx.wcd` is 14 files of 38-byte records (one per weapon): `+0 u16 page`, `+2 u16 weapon id`,
`+4 u16 upgrade parent`, then four materials `{u16 item, u8 quantity, u8 flags}`. The native equipment category is
`file number + 7` (GS file 00 = 7, HBG file 04 = 11, HH file 12 = 19 — confirmed against the game's own lookups).
A weapon's fusion category is the category of its key material (flags 2), else its most valuable categorised
material; some weapons have two recipes (an easier and a harder tier) and both are candidates. 1029 weapons get a
category, 140 categories, 29 weapons have no usable material and are skipped.

### The native box filter

The fusion box builds a byte array of selectable slots by calling `f_340b7c` per entry and storing **1 only when the
returned mask is exactly 1** (`cmp r0, #1` at `0x34a974`). The native mask for a weapon is `0x13` (weapon bit `0x2`,
armour-only progression gate `0x10`), so weapons were refused with "You cannot fuse weapons.". The mod's IPS
rewrites that function so weapons no longer take the armour path; here the same result is reached from the
`FN_VALID` hook: a weapon the catalog knows is returned as **1** (plus the native "already fused" bit `0x80`, so a
fused weapon is refused like a fused armor piece and stays in the separation list). That makes it selectable as
base and appearance and keeps the native pair result at 1.

### Live sequence used

Smithy (Pokke, `tp 1160.35 -699.57 -49.19 145.5`) → A (talk), A, A (menu) → DOWN x4 → A (Armor Fusion) → A
(Fuse Armor) → A (base = box slot 0, a Great Sword) → RIGHT → A (appearance = the next Great Sword) → the native
material picker opens ("Brachydios", 0/10). Box navigation moves 10 slots per UP/DOWN, one per LEFT/RIGHT.

### The stats panel and the 3D preview

The native fusion screen's preview only knew armor: the equipment iteration in `f_863960` walks types 1..5
(`mov r1,#5`) and the validity check in `f_8645cc` refuses weapons, so a highlighted weapon showed "(None)" and the
hunter model kept the armor. Two code patches (three words: `mov r1,#0x15` at `0x863a80`, `mov r0,#1` at
`0x8646f0`/`0x864710`) widen the range and force that check; the stats panel then shows the real weapon and the
model wears it. They are the Weapon Transmog mod's patches, applied by the fork itself through the mods patch queue
(`mods_builtin_code_patch` from `armor_fusion_preview_patches`, before `mods_patch_code`), so the two functions run
in the interpreter. Verified live: "Cheda Blade / Lv 2 / Attack 150 / Sharpness / Slots / Affinity 10%" and the
hunter carrying the weapon in the preview.

### Text

The smithy row is our own string ("Equipment Fusion"). The native screen's rows come from
`Lb_LaboratoryMsg_*` (GMD v2.3 in `nativeNX/<lang>/arc/village/common_<lang>.arc`): after the header, the
16-byte name and the key tables, the file ends with the key-name pool (header `+0x1c` bytes) and the message text
pool (header `+0x20` bytes), the texts being NUL-separated strings in message order. So rewording a message is a
pool edit plus the size field - nothing else moves. `tools/fusion_text.py` does that for English in the build's
`data/romfs` from the player's own dump (`make data`, stamp `data/.fusion-text`) and repacks the archive into a
new file, so the hard links to the dump are never written through. It gives "Fuse Equipment", "Undo Equipment
Fusion", the "or Weapon" box headings and the equipment help lines. The Weapon Transmog addon is not needed (its
romfs was the earlier, temporary source of these texts).

### Still open

- Cross-rank cases (LR weapon + G-rank appearance and the reverse) were exercised only by the user's own run; the
  planner has unit tests for the rule but no in-game LR/HR/G matrix yet.
- The picker's quantity panel is drawn by the game's own widget; the earlier "not visible" note turned out to be
  timing in the scripted runs (the points do get added).
