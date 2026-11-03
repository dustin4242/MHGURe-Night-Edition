# Auto Weapon/Armour Class — Research Notes

Reverse-engineering notes for the "modern MH" feature that lets any armour be equipped
regardless of weapon, while the armour's *stats* follow the class (Blademaster / Gunner) of
the equipped weapon. Appearance (model / name / colour / transmog) stays exactly as the
player set it.

> **Status, 2026-11-03:** the feature is implemented and class-incompatible pieces stay
> equipped (per-frame restore), but the caller analysis in section 3 shows that the lookup
> rewrite currently reaches only the transmog/model resolvers, not the stat calculation, which
> resolves through `f_33f03c`. Whether the derived defence and skills therefore follow the
> weapon class is open; `MHGU_ARMOR_DEBUG=1` (section 6) is the test that settles it.

This covers the three files that make the feature work:

| File | Role |
|------|------|
| `runtime/hle/armor_class.c` | Runtime HLE hook + per-frame maintenance |
| `runtime/hle/armor_pairs.h` | Generated Blademaster↔Gunner counterpart table (committed) |
| `tools/gen_armor_pairs.py` | Regenerates `armor_pairs.h` from the game's `armorSeriesData.asd` |

Wiring:
- `runtime/main.c:50` calls `armor_class_init()` (installs the function hook).
- `runtime/ui.c:100` calls `armor_class_frame()` once per host frame (after input/cheats).

---

## 1. The data source — `armorSeriesData.asd`

Path: `mhgu/romfs/nativeNX/table/armorSeriesData.asd` (git-ignored; not committed).

Layout:
- **8-byte header**, then a flat array of **127-byte records**, one per armour *series*.
- Record count = `(filesize - 8) / 127` = **1287** series (`ARMOR_SERIES_COUNT`).
- Per-record fields of interest:

| Offset | Meaning |
|--------|---------|
| `+0x04` (low byte) | Series id (matches the record index) |
| `+0x0b` | Defence-related byte — **differs** between a Blademaster/Gunner pair |
| `+0x0f` | Class byte A |
| `+0x10` | Class byte B |

Class encoding from the two bytes `(+0x0f, +0x10)`:

| `(+0x0f, +0x10)` | Class |
|-------------------|-------|
| `(1, 0)` | Blademaster-only |
| `(0, 1)` | Gunner-only |
| `(1, 1)` | Both classes |
| `(0, 0)` | Neither |

Example (arms, confirmed by poking):
- series **24** = *Jaggi Guards* → `(0,1)` **Gunner-only**, defence byte `+0x0b = 4`
- series **23** = *Jaggi Vambraces* → `(1,0)` **Blademaster**, defence byte `+0x0b = 3`

The paired records are otherwise nearly identical, which is what makes automatic pairing
possible.

---

## 2. `armor_pairs.h` — the counterpart table

`runtime/hle/armor_pairs.h` is **generated and committed** (it does not exist without
running the generator). Two parallel arrays indexed by series id:

```c
static const uint8_t  armor_series_class[1287]; // 0 both/neither, 1 Blademaster, 2 Gunner
static const uint16_t armor_series_pair[1287];  // other-class counterpart, 0xffff if none
```

### Generator — `tools/gen_armor_pairs.py`

```
python3 tools/gen_armor_pairs.py [armorSeriesData.asd] [output.h]
# defaults: mhgu/romfs/nativeNX/table/armorSeriesData.asd -> runtime/hle/armor_pairs.h
```

Algorithm:
1. Read records, classify each by `(+0x0f, +0x10)`.
2. For every Blademaster series, pair it with the **Gunner series whose records differ in the
   fewest bytes over offsets `4..126`** (the class bytes and a handful of stat bytes differ;
   everything else matches).
3. Refuse to emit if any pairing is ambiguous.

Note: the generator reads the on-disk `.asd`. The committed `armor_pairs.h` reflects the
original (unmodified) data, so pairing/class info is baked in and does **not** depend on the
game table at runtime.

### Verification of the shipped table

Re-running the same matching over the `.asd` and auditing the committed header:

- 611 Blademaster-only + 611 Gunner-only + 65 both/neither → **611 pairs**, 65 `0xffff`, and the
  65 unpaired entries are exactly the both/neither set. Every pair is symmetric.
- Hamming distance over `4..126`: min 5, mean 10.5, max 27; histogram peaks at 8 (115 pairs) and
  9 (149), then a tail out to 27.
- **Zero tied runner-ups** on this data set, so the "refuse on ambiguity" branch never fires for
  the shipped table — it only guards a future data update.
- Bytes that differ, with pair counts: `+4` (id) 611/611, `+0x0f`/`+0x10` (class) 611, `+106`
  611, `+107` 609, then `+0x0b` (defence) 336, `+30` 314, `+0x17` 189, `+0x18` 138, `+0x21` 135.
  That is the expected shape: same series, other class, a handful of stat bytes apart.
- Example: series **23** *Jaggi Vambraces* (`(1,0)`, defence byte 3) ↔ series **24** *Jaggi
  Guards* (`(0,1)`, defence byte 4).

---

## 3. Runtime hook — `f_33c208` (equipment-data lookup)

`FN_EQUIP_DATA = 0x33c208`

Guest signature (AAPCS): `f_33c208(manager /*r0*/, type /*r1*/, id /*r2*/) -> element of the
per-type equipment table`. It is not a free-standing "give me the data row" helper but a typed
container fetch: it validates `type-1 <= 0x17`, picks the container pointer for that type out of
the manager (`+0x3c`, `+0x1d0`, `+0xf8`, `+0x50..+0x84`, `+0x1d4`), rejects `id >= LD32(container
+ 0x64)`, then returns the element (8-byte stride for the common types, via `+0x1c`; a few types
are tail-called to `f_33eef8` / `f_327bb8` / `f_32e4cc`). `r2` is therefore a plain array index,
so rewriting it picks a different record of the same table — which is exactly why the trick also
changes the class/defence/skills of that element, and why the caller gate exists. There are
**85 call sites in 65 functions** across the binary.

- Armour types are **1..5** (head, chest, arms, waist, legs).
- `equip_data` intercepts the call. Before the original runs, if the requested `id` is a
  class-split armour whose class does **not** match the weapon's current class, it rewrites
  `r2` from the real id to the **counterpart id**, so the element returned is the counterpart's
  (its defence/skill bytes, and its model when the piece is transmogged). The real item entry is
  never modified — only the *lookup id*. How far that actually reaches is the subject of the
  caller filtering below.

### Caller filtering (important)

The swap is applied **only when the caller is in `0x2d0000 .. 0x2e3000`**. The caller is
recovered from the link register via `dispatch_function_at(c->r[14], &next)`, which returns the
nearest function entry at or below the address — so the gate is a range of *function entries*,
not a return-address allowlist.

#### What the gate actually covers (re-derived from `generated/main`)

Every `f_33c208(...)` call in `generated/main/*.c` was extracted and cross-checked against the
image (the address never appears as a data word in `main`, so there are no vtable/indirect
callers and the census is complete). **85 call sites in 65 functions.** By caller band:
`0x02`×3, `0x03`×14, `0x04`×2, `0x05`×1, `0x07`×20, `0x08`×16, `0x09`×9 — and exactly **three**
inside the gated band:

| Alias happens in | Call site | What the function is |
|---|---|---|
| `f_2db0b0` | `0x2db138` | transmog/model resolve |
| `f_2db160` | `0x2db1e8` | transmog/model resolve |
| `f_2db210` | `0x2db254` | transmog/model resolve + slot-type cross-check |

All three are small resolvers, not stat summations: `f_33f03c(manager, entry)` to resolve the
entry, `vtable[+0xf0] == 1`, `LD16(entry+8) != 0` (the transmog id), `vtable[+0xf4] == 1`, then
`f_33c208(manager, (entry+4 >> 5) & 0x1f, entry+8)` and `vtable[+0xf0]` on the result.
`f_2db210` additionally compares `vtable[+0x18]` against the slot range `1..5`. Their 21 callers
all sit in `0x3x` (4), `0x4x` (1), `0x7x` (3) and `0x8x` (2) — box/status/data-accessor code.
None of them is in `0x2d/0x2e`.

The equipment/stat code *does* reach `f_33c208`, but only at **depth 2 through `f_33f03c`**:

```
0x2db028  -->  f_33f03c (LR 0x2db044)  -->  f_33c208 (LR 0x33f078)
```

At the inner call `c->r[14]` is inside `f_33f03c`, so `dispatch_function_at` returns `0x33f03c`
— **outside** the band, so the rewrite never fires. This holds for all 59 in-band functions that
reach the lookup that way (`0x2db028`, `0x2db66c`, `0x2dbd2c`, `0x2dbe74`, `0x2dbf78`,
`0x2ddd48`, …), and `f_2db028`'s own body confirms the shape: resolve `entry -> object` through
`f_33f03c`, then read a per-piece value from `vtable[+0x74]`.

So the gate aliases the **transmog/model** path — which is why `clear_auto_tm()` is needed: the
alias auto-applies the counterpart's appearance and the frame pass undoes it — and it does
**not** alias the stat-recalculation path. Earlier revisions of this note listed `0x2db028`,
`0x2db66c`, `0x2dbd2c`, `0x2dbe74`, `0x2dbf78`, `0x2ddd48` as "confirmed active callers" and
`0x2db138`, `0x2db1e8`, `0x2db254` as "listed sites"; the first six are depth-2 *consumers* of
`f_33f03c`, not callers of `f_33c208`, and the last three are call sites *inside* the three
resolvers. The two levels had been conflated.

Consequence: either the hunter's defence/skill contribution keeps the piece's own class values
(the header comment of `armor_class.c` over-promises), or the stat code consumes data that an
aliased lookup cached somewhere else. `MHGU_ARMOR_DEBUG=1` settles it: `equip_data` logs one
`[armor] lookup caller=... type=... id=... in_range=... swapped=...` line per distinct
caller/decision, so grepping for `in_range=1` gives the census of paths actually aliased in the
current build. If no `0x2d` caller appears, the fix is to hook the level that serves the stat
recalc (`f_33f03c`), **not** to widen this range — accepting `0x33f078` would also alias every
identity/name/model lookup that goes through `f_33f03c`.

Other call-site ranges are deliberately **left alone** so the item's identity, name, model and
transmog stay real: the `0x03`/`0x04`/`0x05`/`0x07`/`0x08`/`0x09` accessors (including the
`0x7x`/`0x8x` item box and status screens) and the `0x3x`/`0x4x` box helpers.

```c
static void equip_data(Cpu *c) {
    uint32_t type = c->r[1], id = c->r[2], out = id, caller = 0, next = 0;
    bool armour = type >= 1 && type <= 5 && id < ARMOR_SERIES_COUNT, in_range = false;
    if (armour) {
        caller = dispatch_function_at(c->r[14], &next);
        in_range = caller >= 0x2d0000u && caller < 0x2e3000u;
        if (in_range) {
            cls  = armor_series_class[id];
            want = weapon_class();                 // 1 = blademaster, 2 = gunner
            if (want && cls && cls != want) {
                pair = armor_series_pair[id];
                if (valid(pair) && armor_series_class[pair] == want) out = pair;  // swap lookup id
            }
        }
        log_lookup(caller, type, id, out, in_range);   // debug census, see section 6
    }
    if (out != id) c->r[2] = out;
    fnhook_call_original(c, FN_EQUIP_DATA);
}
```

Because "Both classes" armour has `cls == 0`, it is never aliased — its own stats are correct
for either weapon class, so no scaling is needed.

---

## 4. Weapon class detection

### Player struct

| Symbol | Value | Meaning |
|--------|-------|---------|
| `G_SPLAYER` | guest `0x1a85b58` | pointer to `sPlayer` (the local player) |
| player base | observed `~0x199b2090`, range-checked `[0x08000000, 0xa0000000)` | |
| `PLAYER_WEAPON` | `+0x94` | the **currently drawn** item entry (see gotcha below) |
| `PLAYER_ARMOR` | `+0xc0` | head entry; chest/arms/waist/legs follow at `+0x2c` stride |
| arms box-link | `+0x88` | box-slot link used by the equipment-box "E" marker |
| `ITEM_PTR` | `0x01936514` | pointer stored at the start of every equipment entry |

Equipment **entry** layout (44 bytes / 11 words):

| Offset | Content |
|--------|---------|
| `+0x00` | `ITEM_PTR` (validity marker) |
| `+0x04` | `type` in low 16 bits, `id` in high 16 bits |
| `+0x08` | transmog id (u16) |
| rest | box / equipped state |

`armor_entry()` additionally requires `type == slot_index + 1` (head=1 … legs=5) and `id != 0`.

### Weapon categories (`type` field, low 16 bits of `+4`)

Confirmed by poking values into `+0x94` and reading what `current_weapon_type()` reports:

| type | Weapon | Class |
|------|--------|-------|
| 7  | Great Sword | Blademaster |
| 11 | bowgun family | **Gunner** |
| 19 | Hunting Horn | Blademaster |
| 83 | sheathed/none placeholder (`id=1`) | not a weapon |

`is_gunner_type()` treats **11, 12, 13, 17** as gunner (light/heavy bowgun, bow); everything
else in `1..21` is blademaster.

### Gotcha: `+0x94` is the *drawn* item, not the *equipped* weapon

While the weapon is **sheathed** (e.g. inside the item box), the `+0x94` entry reads the
placeholder `+4 = 0x00010053` (`type=83, id=1`). This is what the very first bug report hit:
the user had a level-3 **Petrified Hunting Horn** (category 19 = blademaster, upgrades keep
the category and only raise the id), but while browsing the box the drawn entry was the
placeholder, so `current_weapon_type()` returned 0 and the whole swap was skipped — the
Gunner-only Jaggi Guards kept their own class/defence and couldn't be equipped.

### The fix — cached weapon class (`weapon_class()`)

```c
static uint8_t g_weapon_class;      // 0 unknown, 1 blademaster, 2 gunner
static uint8_t weapon_class(void) {
    uint16_t type = current_weapon_type();
    if (type) g_weapon_class = is_gunner_type(type) ? 2 : 1;
    return g_weapon_class;          // keeps last known class while drawn entry is a placeholder
}
```

Once a real weapon has been read (e.g. the Horn drawn in the field), the class is remembered
and reused during sheathed/placeholder moments, so the armour swap no longer depends on the
weapon being physically in hand at lookup time. A level-3 weapon is recognized the same as
its base because only the *id* changes, not the category.

---

## 5. Per-frame maintenance — `armor_class_frame()`

The game **strips class-incompatible armour** from the equipped set when the weapon's class
changes. `armor_class_frame()` puts those pieces back so appearance + transmog are preserved:

- For each of the 5 armour slots:
  - If an entry is present: clear any auto-transmog that points at the counterpart, and
    snapshot the whole 44-byte entry into `g_saved[i]` (re-snapshot when the id changes).
  - If the entry vanished but `g_saved[i]` is valid: write the saved 44 bytes back (restores
    the piece the game removed). Restoring the arms slot also restores `sp+0x88` (the box-slot
    link) so the box "E" marker stays correct.
- `clear_auto_tm()` zeroes a transmog field (`+8`) if it equals the counterpart id, so the
  alias in `equip_data` doesn't double-apply the swap to the appearance.

Class is taken from `weapon_class()` (cached), so maintenance is stable while sheathed.

---

## 6. Debug switches

| Env var | Effect |
|---------|--------|
| `MHGU_ARMOR_DEBUG=1` | Logs `[armor] weapon type=NN class=blademaster/gunner` on weapon change; logs `[armor] unknown weapon entry: +4=... (type/id)` for any out-of-range weapon entry (catches a genuinely unrecognized weapon); logs one `[armor] lookup caller=... type=... id=... in_range=... swapped=...` line per distinct caller/decision for armour lookups (bounded at 64 lines) — this is the caller census used to re-derive the gate after a rebuild |
| `MHGU_LOG_FILES=1` | (fs service) logs every romfs file read — useful to find when `armorSeriesData.asd` is loaded |

Run:
```bash
cd build
MHGU_ARMOR_DEBUG=1 ./mhgurecomp
grep '\[armor\]' log.txt
```

Startup note: the first frame can sit idle for ~30–60 s while the **RomFS image index**
(12.6 GB / ~35,531 files) is built. That is not a hang; wait for the `[romfs]` log line.

---

## 7. Control-console aids used during RE

The runtime control pipe (`MHGU_CONTROL=ctl.txt`) supports:
- `peek <addr> [words]` — dump guest memory
- `poke <addr> <hexword>` — write a guest dword (used to set `+0x94` to specific weapon `type|id<<16`)
- `find <hexbytes>` — scan committed guest memory for a byte pattern

Poking weapon words into `sPlayer+0x94+4` (`0x199b2128` at the observed base) confirmed the
weapon-category table above and the sheathed placeholder behaviour.

---

## 8. Open questions / cautions

- **Scope of the alias (open)**: the gate fires only inside `f_2db0b0` / `f_2db160` /
  `f_2db210`, i.e. the transmog/model resolvers, and never for the stat-recalculation path,
  which resolves through `f_33f03c` (see section 3). Whether the hunter's derived defence and
  skills therefore keep the piece's own class values is the one thing this note cannot settle
  statically — `MHGU_ARMOR_DEBUG=1` plus a status-screen A/B answers it in one session. If the
  stat path must be aliased, hook `f_33f03c`; do not widen the caller range, because every
  identity/name/model lookup goes through it too.
- **Equip restriction path**: with the alias reaching only the model/transmog path, the "can I
  equip this?" side is most plausibly carried by the per-frame restore (`armor_class_frame`,
  which re-writes stripped pieces) plus the cached weapon class, not by the lookup rewrite. If a
  piece ever fails to equip while its class *is* cached correctly, log with `MHGU_ARMOR_DEBUG=1`
  and check which callers appear — the census will show whether the restriction path is aliased
  at all.
- **Caller-range fragility**: the `0x2d0000..0x2e3000` range is tied to the current build's
  `main_012` layout. A rebuilt/updated `mhgugame.so` could move these addresses; the alias
  would silently stop applying. The `[armor] lookup ... in_range=1` lines are the regression
  test — if none appear while swapping a class-split piece, the range is stale.
- **Player base is dynamic**: `G_SPLAYER` (`0x1a85b58`) is a stable pointer *cell*, but the
  `sPlayer` object address varies; always range-check rather than hard-coding `0x199b2090`.
- **Weapon category set** (`11/12/13/17` = gunner) is inferred, not exhaustively proven for
  every weapon. The debug log's `unknown weapon entry` line will reveal any category that the
  current `1..21` gate mishandles.
- **`armor_pairs.h` is committed, `.asd` is not.** After a game-data update, regenerate the
  header; the runtime never reads the `.asd` directly.

---

## 9. Quick reference — end-to-end flow

```
gen_armor_pairs.py  --(.asd)-->  armor_pairs.h (class[] + pair[])
                                          |
game calls f_33c208(mgr, type 1..5, id)   |   equip_data hook
   caller in 0x2d0000..0x2e3000?  yes ----+--> want = weapon_class()  (cached weapon class)
                                            if cls[id] && cls[id]!=want: r2 = pair[id]
                                          no  --> leave real row (identity/name/model)
   (only f_2db0b0 / f_2db160 / f_2db210 match; the 0x2d stat code reaches the lookup through
    f_33f03c and therefore keeps the real row -- see section 3)
armor_class_frame()  -> re-equip stripped pieces, fix arms box-link, clear auto-transmog
```
