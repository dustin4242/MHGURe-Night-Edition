# Importing a brand new monster: a possible route

Follow-up to `docs/MONSTER-LOADING.md`. That document maps what the game loads; this one is the plan for putting a
new monster in. It is a route, not a promise: several formats have to be written, not just read, and the engine has
final say on whether it accepts them.

## What "a new monster" can mean

Four levels, each a smaller step than the next:

1. **Reskin** - a new mesh and textures on an existing monster's skeleton, animations, hit data and AI. The monster
   looks new, behaves like the one it replaces.
2. **New motions** - the same rig, new idle/walk/attack animations (needs a motion writer).
3. **New identity** - its own `em<ID>` archive, its own quest entry, name, size and stats.
4. **New behaviour** - new AI/attacks (`cmd_tbl`, `attackdata`, `hitsize`), the deepest reverse engineering.

The route below goes 1 -> 3 and treats 4 as a later option. A reskin already needs every asset pipeline (model,
textures, materials, packaging), so it is the right first target.

## The constraint that shapes everything

The game's own engine parses its assets. The port does not replace that: it hands the game a RomFS, lifts the NVN
shaders to GLSL and runs the draw stream. So "importing" means **writing the game's native formats**, not teaching the
port a new one. The port still helps a lot: it is where the debug hooks, file logging and, if needed, new loader
hooks go.

Quests reference a monster by name: `quest/boss/setEmMain/b_m<slot>em<ID>_<var>`, and the engine builds the archive
path (`arc/enemy/em<ID>_<var>.arc`) and the entry paths inside it (`enemy/em<ID>/em<ID>_<var>/...`) from that name.
That gives two strategies:

- **replace an existing ID** (no quest/UI work; the game sees the same monster, new body and motions);
- **add an ID** (rename every entry to the new ID, edit a quest's `setEmMain` name, add the name/size/stat rows). More
  work, but no base monster is overwritten.

## The formats to write

| Format | Read today? | To write | Difficulty |
| --- | --- | --- | --- |
| `ARC` v17 | yes (`tools/mtarc.py`) | pack/repack | **done** |
| `TEX` | header understood (mip sizes, BC7-class 8 bpp data) | BC7 encoder + mips + header | medium (encoders exist) |
| `MRL` | header + texture paths understood; material records not | copy the base material records, change texture paths | easy for a reskin |
| `MOD` | not parsed | mesh, vertex streams, skin weights, node/bone table, LODs | **hard**, the crux |
| `LMT` | header, 31 motions per file, offsets seen; curve format not | keyframe curves per bone | hard |
| data (`HDE`, `HTS`, `BDD`, `DTB`, `DTT`, `EMC`, `XFS`, `MTD`) | magic only | copy the base monster's files unchanged | no work for a reskin |
| sound | paths inside the archive | copy the base monster's, renamed if the ID changes | easy |

The good news: for a reskin **only `MOD`, `TEX` and the `MRL`'s texture paths change**. Hit data, AI, animations,
difficulty, area behaviour and sound can all be the base monster's. The model has to be bound to the base monster's
skeleton (same bone indices/hierarchy), so animations and hitboxes keep working.

## Phases

### Phase 0 - archive surgery (days)
Prove the game accepts a repacked monster archive at all: extract `em085_00.arc`, change one recoverable thing
(a texture-set path in the `MRL`, or an entry's bytes), repack, drop it in `mods/<name>/romfs/nativeNX/arc/enemy/`,
run quest 206 and screenshot. This validates `mtarc.py pack`, the mods layer and `arcmerge` for enemy archives, and
finds out whether the engine checks sizes/hashes.

### Phase 1 - textures (days)
A small TEX tool: decode an existing TEX, re-encode the same pixels (proves header/mip writing), then a BC7 encoder
(or CLI: `compressonator`, `basisu`) for new art, repack and see the monster in new colours. No model changes yet, so
every failure is a texture failure.

### Phase 2 - model export (weeks, the main risk)
Build a MOD parser first (dump nodes/bones, vertex streams, indices, weights, LODs from `em085_00`), then a writer that
**re-emits the base MOD with a replacement mesh on the same skeleton**. Validate by round-tripping an untouched model
(should look identical). Then export a new mesh (Blender -> FBX/glTF -> MOD) skinned to the base bones. This is where
most of the effort goes and where a failure looks like a crash, a T-pose or an invisible monster.

### Phase 3 - materials (days to weeks)
For the reskin, copy the base `MRL` and point its texture-set paths at the new `TEX` files - no material record work.
If the new monster needs different material parameters (colours, alpha, emission), the `MRL` records have to be
decoded first (see `MONSTER-LOADING.md`); until then, bake the look into the textures.

### Phase 4 - new motions (weeks)
Reuse the base motions for the reskin. New motions need an `LMT` writer (curve keys per bone) or a retarget of motions
from another monster with the same rig; the `LMT` header/offsets are known, the curve payload is not yet decoded.

### Phase 5 - its own identity (days to weeks)
Duplicate the archive under a new ID, rename every entry (`em<old>` -> `em<new>`), patch the `MRL` texture paths, add a
quest whose `setEmMain` names the new monster, and give it name text, a difficulty row (`em_nando_tbl`) and size data.
Depends on whether the engine accepts an unknown ID (the loader builds paths from the name, but tables and the
Hunter's Notes may reject it).

### Phase 6 - new behaviour (optional, months)
Decode `cmd_tbl`/`attackdata`/`hitsize` and author new attacks. Everything above can ship without this.

## Tooling to build

- `tools/mtarc.py`: already covers ARC; add a `monster` helper that renames entries for a new ID.
- `tools/tex.py`: TEX decode/encode (template header, BC7 via an external encoder).
- `tools/mod.py`: MOD dump and rewrite - the core deliverable.
- `tools/lmt.py`: LMT dump and (later) rewrite.
- Port-side debug hooks (runtime/hle, `fnhook`): log when the game loads a monster's model/motion, which file and
  what it rejected - this is how a failed experiment becomes a fix rather than a crash.
- A tiny "new monster" mod layout: `mods/<name>/romfs/nativeNX/arc/enemy/em<ID>_<var>.arc` plus the quest archive.

## Acceptance tests

| Phase | Playable proof |
| --- | --- |
| 0 | quest 206 still loads the same monster from a repacked archive |
| 1 | a recoloured monster texture on screen |
| 2 | a mesh swap renders, animations play, hitboxes still land |
| 3 | the reskin's materials look right from all sides |
| 4 | a new attack animation plays and connects |
| 5 | a quest spawns the new ID with its own name in the UI |

## Risks

- **The engine checks nothing we can see, or checks everything**: sizes, hashes, alignment, node counts. Phase 0/1
  find out cheaply.
- **Skeleton binding**: animations and hitboxes are bone/index based; a model that does not match the base rig will
  T-pose, deform wrongly or crash. Start from the base skeleton, not from a fresh one.
- **LODs and parts**: monsters have several parts/visibility groups (`XFS`) and level-of-detail models; a new mesh must
  fill the same slots.
- **Memory/streaming**: the archive load is one file; oversized textures or models will hurt or fail.
- **Name/ID tables**: a new ID may need rows in the shared tables and localized text, which is a separate modding
  surface.
- **No specifications**: every format is inferred from samples; a parser that round-trips the game's own files is the
  only proof of understanding.

## Recommended first step

Phase 0 + 1 on `em085_00` (the monster of quickhunt quest 206): repack the archive, recolour one texture, screenshot
it in-game. That is a self-contained weekend-sized piece that proves packaging, textures and the mods layer before the
MOD writer - the actual long pole - is started.
