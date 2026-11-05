# How MHGU loads monsters: models, textures, animations, quests

Research notes, 2026-11-05. Everything below was checked against the game's own RomFS, the port's tools and a running
copy of the game (`MHGU_LOG_FILES=1` file log, `MHGU_QUICKHUNT` to enter a quest, `tools/mtarc.py` to open the
archives). Where something is inferred rather than verified it says so.

## Short version

A quest is one archive (`romfs/nativeNX/loc/arc/quest/q<number>.arc`) that names the map, the large monsters and the
small monsters. When the quest starts the game reads, in order: the quest archive, the shared quest text/lists, the
map's common archive, the map's area archives, then **one archive per large monster**, `arc/enemy/em<ID>_<variant>.arc`,
which holds that monster's models, textures, animations, hit data, AI tables and carving data. The monster's stats are
picked from a shared difficulty table in `loc/arc/resident.arc`.

## Where the data is

`romfs/nativeNX/` is 12 GB in MT Framework ARC archives (version 17; format and tool: `tools/mtarc.py`).

| Path | Size | What |
| --- | --- | --- |
| `arc/enemy/em<ID>_<var>.arc` | 1.7 GB, 137 archives | one large monster each (models, textures, motions, data) |
| `arc/stage/<map>/<area>.arc` | 4.5 GB, 34 map folders (`m01d`…, `d` day / `n` night) | the maps, area by area |
| `loc/arc/quest/q<number>.arc` | 70 MB, 1857 archives | one archive per quest |
| `loc/arc/quest/`, `vq.arc`, `gq.arc`, `gq2.arc` | | quest text/lists, shared quest assets (`quest.arc`, 9 MB) |
| `loc/arc/resident.arc` | 15.9 MB | shared monster data: difficulty table, size scaling, area tables |
| `eng/arc/resident_eng.arc` etc. | | localized UI textures |
| `table/` | 5.2 MB | item/armor/quest-adjacent tables (no monster parameter tables) |

## The loading sequence (verified)

`MHGU_QUICKHUNT=206` (a village quest with a large monster) with `MHGU_LOG_FILES=1` gives, in order:

```
read nativeNX/loc/arc/quest/q0000206.arc          the quest itself
read nativeNX/loc/arc/quest.arc                   shared quest assets
read nativeNX/eng/arc/quest_eng.arc               localized quest text
read nativeNX/loc/arc/quest/vq.arc                village quest list/text
read nativeNX/loc/arc/stage/map_com/m01.arc       map common data
read nativeNX/eng/arc/stage/map_com/m01_eng.arc
read nativeNX/eng/arc/stage/mapUI/m01d/a00_ui_eng.arc
read nativeNX/arc/stage/m01d/m01d.arc             the map
read nativeNX/arc/stage/m01d/a00.arc              the area the quest starts in
read nativeNX/arc/stage/quest/shell/com.arc       quest-only map pieces
read nativeNX/arc/enemy/em085_00.arc              THE LARGE MONSTER (em085, variant 00)
read nativeNX/loc/arc/resident.arc                shared monster tables
```

So the enemy archive is loaded *lazily by the quest*, not per area, and one archive carries everything for that
monster (the 17 MB of `em001_00.arc` is mostly its own models/textures plus shared effect and sound data).

## Inside a monster archive

`arc/enemy/em001_00.arc` (549 files, 17 MB), grouped:

| Path in the archive | Size | Magic | What it is |
| --- | --- | --- | --- |
| `enemy/em001/em001_00/mod/em001_00_BM.241F5DEB` | 1,400,384 | `TEX\0` | texture container (body) |
| `…/*_BM+A.241F5DEB`, `…/*_NM_MIRROR.241F5DEB` | 701 KB / 1.4 MB | `TEX\0` | the other texture variants |
| `…/mod/em001_00.58A15856` | 357 KB | `MOD\0` | the model: meshes and bones (part names are readable, e.g. `XfB__m01_eye`, `XfBN__E1__m00_body`) |
| `…/mod/em001_00.2749C8A8` | 3,600 | `MRL\0` | material list: references the model variants by path (`enemy\em001\em001_00\mod\em001_00_BM+A`) and the textures |
| `…/mod/em001_00.7CD0E77E` | 1,429 | `XFS\0` | parts visibility/config (`mPartsVisibleGroupList`, `mQuality`, …) |
| `…/mot/em001_00_0.76820D81` … `_4` | 0.5–1 MB | `LMT\0` | the motion lists (5 sets, `_0`…`_4`) |
| `…/mot/em001_00_cam.39C52040` | 47 KB | `LCM\0` | camera animation |
| `enemy/hit_data/em001_00_attackdata.70BB64BA` | 2,064 | `HDE\0` | attack/hit data |
| `enemy/hit_data/em001_00_hitsize.5653C1B0` | 2,024 | `HTS\0` | hit box sizes |
| `enemy/body_data/em001_00_bodydata.1AC7B52D` | 1,468 | `BDD\0` | body/parts data |
| `enemy/cam_body_data/…` | 544 | `BDD\0` | camera-collision body |
| `enemy/dt_base/em001_00_dtbase.59D9C3DA` | 622 | `DTB\0` | difficulty base values |
| `enemy/dt_base/em001_00_dtbparts.1BBC291E` | 292 | `DTP\0` | difficulty per part |
| `enemy/dt_tune/em001_00_dttune.48E8AC29` | 1,594 | `DTT\0` | difficulty tuning |
| `enemy/cmd_tbl/em001_00_cmdtbl.3AABBA02` | 51 KB | `EMC\0` | the AI command/action table |
| `enemy/area_link|area_eat|area_select|area_patrol|dou_mou_ka|map_info/*` | 1–27 KB | `XFS\0` / `MTD\0` | where the monster may go, eat, sleep, patrol; enrage; map markers |
| `enemy/hagi/hagi_s0…s4_em001_00.33A84E14` | 346 each | | carving results per rank |
| `arc/shell/em/em001_00_shell.73850D05` | | | the broken-part ("shell") models |
| `effect/…`, `sound/…` | | | effects and audio for the monster |

The `_00` / `_01` / `_02` / `_04` suffix on the archive is a form/state index (the game ships several archives per
monster id; the exact meaning of each suffix is not verified). Quests reference `<ID>_<variant>`.

### Textures

Textures are **inside the monster's archive** (no separate texture packs). A `TEX` container holds the whole mip
chain: the 1,400,384-byte body texture has a 1,048,576-byte mip 0 (a 1024x1024 texture at 8 bits per pixel), then
256 KB, 64 KB, 16 KB, 4 KB, 1 KB … for the mips - block compression at 8 bpp, i.e. BC7 or BC3 (community MHGU texture
tools convert BC7; the header carries the format code and the mip sizes). Modded textures must keep the same
dimension, format and mip layout, or the game's upload path misreads them.

### Models, materials

`MOD` holds the meshes and the bone/part hierarchy; `XFS` controls which parts are visible (severed tails, broken
parts, quality tiers). The broken parts have their own `shell` model archive.

### Materials (MRL) and shaders

`MRL` (magic `MRL\0`, version 0x22) is the per-model material list. Its header (the values are verified against the
example files; the field meanings are inferred from them and the shader package):

| Offset | Value | Meaning |
| --- | --- | --- |
| +0x04 | 0x22 | version |
| +0x08 | 4 / 1 / 3 | material count (em001 body / em001 tail / em085 body) |
| +0x0c | 3 | texture set count |
| +0x10 | 0xb2d34c60 | **shader package id** - the same value as the MFX header |
| +0x14 | 0x1c | offset of the texture-set references |
| +0x18 | 0x100 | offset of the material table |

Each texture-set reference (0x4c bytes: type hash 0x241F5DEB = TEX, two zero words, a 64-byte path) names a whole
texture file for that model, e.g. `enemy\em001\em001_00\mod\em001_00_BM`, `…_BM+A`, `…_NM_MIRROR` (em085 uses
`NM_MQ_MIRROR`). These are the monster's texture variants - one TEX file each, not per-material slots.

The material table at +0x100 holds one record per material: hashes/ids and parameter data. The hashes did not match CRC32,
FNV1a or Murmur3 of the shader-package names we tried, so material names are likely GUIDs or MT's own hash;
the record layout (a 0x340-byte block appears in the first record) is not decoded yet - that is the next step for
changing colours/parameters.

Shaders are **not in the monster archive**: `system/app_shader/AppShaderPackage.mfx` (MFX version 0x35, 1.3 MB) is the
game-wide material shader database. It holds compiled shader code and the plain-text symbol names of the material
system (`SYSTEM_OUTPUT`, `ALBEDO`, `AMBIENT_OCCLUSION`, `ALPHA_MASK`, `ADHESION_INPUT`, `BilateralBlurHSize12`, …).

At the port level a material is only: the draw's program (translated VS/FS), its constant blocks (set 1, binding < 17)
and its textures (set 1, binding 32 + i, in the shader's declared order) - there is no material name. The port lifts the
Maxwell shaders the game builds from the MFX and never parses the MFX itself. It does compute a **shader code hash**
per program (`code_hash`, `runtime/nvn/vulkan/exec.c`; shown by the draw dump as `code hash …`), which is stable across
runs and is the natural key for a shader override.

What can be changed today: textures (replace a TEX, same size/format/mips), the texture-set paths in the MRL header,
and - with the record layout decoded - material parameters. Shader changes mean editing the MFX's compiled code, or
using the port's GLSL override mechanism (today a debug feature, `MHGU_GLSL_OVERRIDE`, keyed by shader address; keyed
by code hash it could become a mod format).

### Animations

The five `LMT` files per monster are motion lists selected *by set* (`_0`…`_4` - the game switches sets by state, e.g.
normal/enraged or weapon matchups; not confirmed which). The engine plays a motion by `(group, index)` through
`0x276154` (`FN_SET_ACTION` in `runtime/hle/emtrace.c`, which can force actions from the control file:
`emact <group> <index> [mode]`), so an animation's identity is the pair of numbers, not the file name.

## Shared monster data (`loc/arc/resident.arc`)

| Entry | What |
| --- | --- |
| `enemy/resident/em_nando_tbl` (4,528 B, magic `NAN\0`) | the difficulty table: header then **141 rows x 8 floats** (HP, attack, defense, …). Row 0 is the default; a quest gives each large monster a row (`mVitalTblNo_N`, `mAttackTblNo_N`, `mOtherTblNo_N` in its cQuestData), and the game nudges the HP row per quest. `runtime/hle/hpscale.c` hooks the row getter (0x2baaac) to scale HP. |
| `enemy/resident/em_resident_dtbase` | base values for the table |
| `enemy/resident/em_size_scale_data`, `em_size_yure_data` | monster size scaling / variation |
| `enemy/area_act_tbl/enemy`, `enemy/area_link/enemy` | shared area behaviour tables |

## Quests

One archive per quest: `loc/arc/quest/q<number>.arc` (e.g. `q0010107.arc` = hub quest 10107, "Tackling the
Tetsucabra"). It contains:

| Entry | What |
| --- | --- |
| `quest/boss/setEmMain/b_m<slot>em<ID>_<var>` | one file per large monster slot: the **file name** is the monster (`em066_00`), the content (32 B) its spawn parameters (first float 1.0, then 1, 0, 3, then four floats that look like a position/heading). `em000` = empty slot. |
| `quest/zako/emSetList/z_m<area>d_<n>` (magic `esl\0`) | the small-monster spawn list for the area |
| `quest/rem/rem_…` | reward item tables |
| `quest/supp/supp_…` | supply box contents |
| `quest/plus/questPlus_…` | quest modifiers |
| `loc/quest/questData/questData_<number>` | the binary quest definition (starts `00 00 4b 43` = "KC"; the quest number as u32 at **+0x0c** — verified 10107; map, time of day, rows, timers, rewards follow) |
| `loc/quest/questLink/questLink_…` | links to the text/list entries |
| `<lang>/quest/questData/questData_<number>_<lang>` | the quest's text as MT Framework GMD (`GMD\0`) |

Shared: `loc/arc/quest.arc` (9 MB: common sounds/assets), `vq.arc` / `gq.arc` / `gq2.arc` (village / hub / G-rank
text and lists, ~4,000 entries each as GMD), `eng/arc/quest_eng.arc` (localized strings).

Runtime: `sQuest *0x1a8ad90` (+0xa4 the cQuestData, +8 the quest number); the quest selector
`sQuestSelector *0x1a8ae38` keeps the ordered quest and the join count (+0xd750). `runtime/hle/quickhunt.c` documents
how a quest departs (selector flags +0xd65, ordered quest +0xd68).

## Runtime monster objects

From `runtime/hle/emtrace.c` and `hpscale.c`:

- `sEnemy *0x1a51fe0`, `+0x1c`: the **10 live enemies** of the loaded zone (an array of pointers).
- an enemy: `+0x1428` status block pointer, `+0x1b6` its network id (the `em<ID>` number), `+0x374` max HP, `+0x40`
 position, `+0x73e0` the current action (group, index).
- `0x276154` starts a monster action `(group, index, mode)` — used by `MHGU_CONTROL`'s `emact`.
- `0x2baaac` returns a difficulty-table row for a monster (column 0 HP, 1 attack, 2 the rest); `hpscale.c` hooks it.
- `runtime/hle/hatestep.c` reads the monsters' attention/aggro scores; `emtrace` logs actions and positions.

## Modding implications

Tools: `tools/mtarc.py list|extract|pack|index` (ARC v17; extract writes an `_arc.json` manifest for repacking, and
missing type hashes can be filled in `mt_types.json`).

Formats needed to change content: `TEX` (textures, BCn + mips), `MOD`/`MRL`/`XFS` (models/materials/parts), `LMT`/`LCM`
(animations), plus the small data files above. Editing the binary data files means decoding their layouts (next step);
text is GMD.

How a mod reaches the game: a folder in `mods/` with `romfs/nativeNX/...`; the port layers it over the RomFS and
merges ARC archives (`runtime/arcmerge.c`). A mod may ship a partial ARC with only the changed entries — the merge
takes the rest from the game, and compares entries by their stored bytes. There is **no per-member override** yet
(`mods/<name>/romfs/<arc>.arc.d/<member>` is planned, not implemented), so today a changed model/texture means a
repacked ARC or a partial one.

Recipes that work today:
- **Swap one texture**: extract `em<ID>_<var>.arc`, replace the `TEX` file with one of the same size/format/mips, pack
  it, drop it in `mods/<name>/romfs/nativeNX/arc/enemy/`.
- **Point a quest at another monster**: the monster is the *file name* under `quest/boss/setEmMain/` — renaming it in
  the quest archive (repack) changes which monster the quest spawns, if the rest of the quest data agrees.
- **Change stats**: edit the `dtbase`/`dttune` data (binary layout not decoded yet) or use the port's `[Monster HP]`
  setting, which scales the difficulty row at runtime.

## Open questions / next steps

1. Decode `questData` (`KC`) and the `dtbase`/`dttune` (`DTB`/`DTT`) layouts — the game's accessors are known entry
   points (0x2baaac for difficulty rows; the quest parser still to be located).
2. Which motion set (`_0`…`_4`) is active when, and how the AI's `cmd_tbl` (`EMC`) maps to `(group, index)`.
3. Where the Hunter's Notes monster names/descriptions live (the localized GMD tables; not found yet — they are not in
   the enemy archives).
4. Per-member ARC overrides (`.arc.d`) would make monster mods practical without repacking whole archives.
5. How the game streams enemies beyond the 10 live slots (areas with many monsters).
