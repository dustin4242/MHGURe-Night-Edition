# Auto Weapon/Armour Class — Research Notes

Reverse-engineering notes for the "modern MH" feature that lets any armour be equipped
regardless of weapon, while the armour's *stats* follow the class (Blademaster / Gunner) of
the equipped weapon. Appearance (model / name / colour / transmog) stays exactly as the
player set it.

> **Status, 2026-11-04: working, verified live in both directions.** With a Petrified Hunting
> Horn equipped, both the Gunner-only *Jaggi Guards* (arms, series 24) and the Blademaster
> *Jaggi Vambraces* (arms, series 23) can be equipped from the item box, and each one really
> lands in `sPlayer` (`arms = 0x00180003` / `0x00170003`) with the other four armour pieces
> untouched. The root cause was **not** the caller gate and **not** a "sheathed placeholder":
> it was a missing 5-bit mask when reading the weapon category out of the entry's `+4` word —
> see section 4, **"Root cause and fix"**. The earlier 2026-11-03 census in section 6 that
> concluded "the alias never fires" was taken before that mask was added.
>
> Known limit that remains open: the alias rewrites the *equipment-data lookup*, so the equip
> gate accepts the piece; whether the derived defence/skills of a swapped piece follow the
> weapon class resolves through `f_33f03c` (sections 3 and 8) and is still unmeasured.

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
— **outside** the band.

> **Corrected 2026-11-04:** this static census under-counts. The live census (section 6) shows
> an in-band, *id-carrying* lookup from `0x2db66c` (`type=3 id=24 in_range=1 swapped=00000017`),
> which is the equip gate and the reason the feature works. `f_2db66c` evidently also calls the
> lookup directly (or through a path the extraction attributes elsewhere), so treat the
> "exactly three in-band sites" figure as a lower bound and re-derive it from the runtime census
> after any rebuild rather than from `grep` alone. This holds for all 59 in-band functions that
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
| `+0x04` | bits 0..4 `type`, bits 5..9 **upgrade level** (0 = Lv 1), bits 16..31 `id` — mask the type with `0x1f` |
| `+0x08` | transmog id (u16) |
| rest | box / equipped state |

`armor_entry()` additionally requires `(type & 0x1f) == slot_index + 1` (head=1 … legs=5) and `id != 0`.

> **The level lives in bits 5..9 of `+0x04`** and is easy to lose: a level-2 Jaggi Helm stores
> `0x00170021` — id 23, and `type` 33 = `1 | 1 << 5`. Masking with `0x1f` is what makes an upgraded
> piece a valid slot; without it `armor_entry()` rejects the piece, the snapshot never captures it, and
> the restore puts back whatever older set was captured instead (the reported "my Lv 2 helm shows
> Lv 1, and the whole set went back to Leather" bug, 2026-11-04), while a stripped upgraded piece is
> never put back at all (the "vambraces show None" bug).

### Weapon categories (`type` field — **5 bits**, not 16)

The `type` field is only the **low five bits** of the `+4` word; bits 5..9 hold a second field
(the weapon's level-ish value: `2` for the Petrified Horn, `0` for the Bone Horn, `0` for the
Arbalest). The game itself masks before it asks for equipment data — `f_7b973c` calls
`f_33c208(mgr, LD16(item+4) & 0x1f, LD16(item+6))` and `f_7b91e0` tests `LD8(item+4) & 0x1f` —
so anything reading the category must mask the same way.

| `+4` word | weapon | type (`& 0x1f`) | field 2 (`>>5 & 0x1f`) | Class |
|-----------|--------|-----------------|------------------------|-------|
| `0x001a0007` | Bone Cleaver | 7 | 0 | Blademaster |
| `0x000f000b` | Arbalest (LBG) | 11 | 0 | **Gunner** |
| `0x00010053` | Petrified Hunting Horn | **19** | 2 | Blademaster |
| `0x00140013` | Bone Horn | 19 | 0 | Blademaster |

`is_gunner_type()` treats **11, 12, 13, 17** as gunner (light/heavy bowgun, bow); everything
else in `1..21` is blademaster. Note `0x00010053` read *unmasked* is `0x53` = 83 — which is
exactly the value that used to be mistaken for a "sheathed placeholder".

### Root cause and fix (2026-11-04)

`current_weapon_type()` used to do

```c
uint16_t type = LD16(weapon + 4);          // wrong: 0x0053 = 83 for the Petrified Horn
if (type >= 1 && type <= 21) return type;
```

so for the report's Petrified Hunting Horn (`+4 = 0x00010053`) it returned **83**, the `1..21`
guard rejected it, `weapon_class()` left `g_weapon_class == 0`, `equip_data()` therefore never
aliased, and the Gunner-only Jaggi Guards were resolved as their real Gunner row and refused by
the equip gate. There was never a "placeholder" state: `+0x94` always holds the *equipped*
weapon entry, it just needed masking. The fix is one line:

```c
uint16_t type = (uint16_t)(LD16(weapon + 4) & 0x1fu);   // mask first, then range-check
```

with the range check `if (type >= 1 && type <= 21)` unchanged. Nothing else changed: the entry
is never written, the save is untouched, and the swap stays unconditional (no ini key).

Evidence from the live run with the Petrified Horn equipped (log line from the in-band equip
gate, caller `0x2db66c`):

```
[armor] weapon type=19 class=blademaster
[armor] lookup caller=002db66c type=3 id=24 in_range=1 swapped=00000017
```

Before the mask the same caller logged `swapped=00000000` — i.e. series 24 (Gunner Guards) was
handed to the gate as itself and rejected.

### The class cache (`weapon_class()`) — kept, but no longer load-bearing

```c
static uint8_t g_weapon_class;      // 0 unknown, 1 blademaster, 2 gunner
static uint8_t weapon_class(void) {
    uint16_t type = current_weapon_type();
    if (type) g_weapon_class = is_gunner_type(type) ? 2 : 1;
    return g_weapon_class;          // keeps the last known class over a transient type 0
}
```

With the mask in place a real category is read on the very first lookup (including the
"load save → village → item box" flow), so the cache is only a guard against a momentary
`type == 0` (empty slot / entry not yet populated) inverting the decision mid-frame. A
level-3 weapon is recognized the same as its base because the upgrade only raises the *id*,
not the category.

---

## 5. Per-frame maintenance — `armor_class_frame()`

The game **strips class-incompatible armour** from the equipped set when the weapon's class
changes. `armor_class_frame()` puts those pieces back so appearance + transmog are preserved:

- For each of the 5 armour slots:
  - If an entry is present: clear any auto-transmog that points at the counterpart, and
    snapshot the whole 44-byte entry into `g_saved[i]` (every frame, with the time it was taken).
  - If the entry vanished but `g_saved[i]` is valid, a class change was observed within the last
    second **and** the snapshot is younger than half a second: write the saved 44 bytes back
    (restores the piece the game removed). Restoring the arms slot also restores `sp+0x88` (the
    box-slot link) so the box "E" marker stays correct.

  The class-change and freshness conditions matter: the entry is also cleared for reasons that
  have nothing to do with the weapon class — an armour upgrade at the smithy rebuilds it, and
  taking a piece off clears it. Restoring there handed the game a stale 44-byte entry, whose
  box-record pointer (`+0x28`) still pointed at the pre-upgrade record: an upgraded helmet showed
  its old level (Lv 1) in the item box and the pause menu. Restoring only immediately after a
  class change (and only pieces that were worn just before it) keeps the feature's own case and
  leaves every other case to the game.
- `clear_auto_tm()` zeroes a transmog field (`+8`) if it equals the counterpart id, so the
  alias in `equip_data` doesn't double-apply the swap to the appearance.

Class comes from `weapon_class()`, which is stable even if a lookup lands while the weapon entry momentarily reads `type == 0`.

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

### Live caller census (fresh boot → item box, 2026-11-03)

Reproduced end to end with the control pipe on a hidden run (`MHGU_HIDDEN=1`,
`SDL_VIDEODRIVER=x11`, `MHGU_ARMOR_DEBUG=1`): boot → title → `A` (skip movie) → load save →
village → `X` (World Map) → `A` (Bherna) → `A` (travel) → `STICK_UP 240` (walk to the house) →
`A` (Item Box) → `DOWN`×5 → `A` (Manage Equipment) → `A` (Select Equip.). The complete armour
census for that session, verbatim:

```
[armor] lookup caller=0046ec84 type=1 id=0  in_range=0 swapped=00000000
[armor] lookup caller=002dbe74 type=1 id=0  in_range=1 swapped=00000000
[armor] lookup caller=002dbf78 type=1 id=0  in_range=1 swapped=00000000
[armor] lookup caller=002db0b0 type=1 id=0  in_range=1 swapped=00000000
[armor] lookup caller=002ddd48 type=1 id=0  in_range=1 swapped=00000000
[armor] lookup caller=00487454 type=1 id=0  in_range=0 swapped=00000000
[armor] lookup caller=0048a30c type=2 id=0  in_range=0 swapped=00000000
[armor] unknown weapon entry: +4=00010053 (type=83 id=1) +8=00000000 +c=00000000
[armor] lookup caller=006ea4c0 type=1 id=23 in_range=0 swapped=00000000
[armor] lookup caller=006eb170 type=1 id=23 in_range=0 swapped=00000000
[armor] lookup caller=006eb2fc type=5 id=3  in_range=0 swapped=00000000
[armor] lookup caller=0047b410 type=1 id=23 in_range=0 swapped=00000000
[armor] lookup caller=0076ea38 type=1 id=3  in_range=0 swapped=00000000
```

What the run establishes:

- **The in-band callers are real but only ever ask for `id=0`.** Four `0x2dxxxx` sites
  (`0x2dbe74`, `0x2dbf78`, `0x2db0b0`, `0x2ddd48`) match the gate, so the range is not stale —
  but they fire during boot with an empty slot. Two of them (`0x2dbe74`, `0x2dbf78`, `0x2ddd48`)
  are not the three sites the static scan predicted (`0x2db138`, `0x2db1e8`, `0x2db254`), so the
  static audit under-counts in-band callers.
- **Every real armour id arrives out of band.** `id=23` (head) and `id=3` (chest/legs) come
  from `0x6ea4c0`, `0x6eb170`, `0x6eb2fc`, `0x47b410`, `0x76ea38` — i.e. the identity/model/UI
  path, all `in_range=0`. So the path that draws the hunter is *not* aliased, which matches
  §3's conclusion.
- **`swapped=` was `00000000` on every line for the whole session**: the alias never fired. Two
  independent reasons, both confirmed above — the class cache was 0 (weapon never drawn) and the
  callers carrying real ids are out of band. A `swapped=0000xxxx` line is the evidence that the
  alias actually applied, and none appeared.
- **The observed save could not exercise the swap anyway:** 1/10 equipment-box pages are
  populated, the box holds no spare armour, and the hunter wears a class-consistent blademaster
  set (head 23, chest 3, arms 23, waist 3, legs 3) with a level-3 *Petrified Horn*. Demonstrating
  the alias live needs either purchased armour or guest-memory pokes.

> **Superseded (2026-11-04).** The census above was taken *before* the `& 0x1f` mask
> (section 4) existed, so its "the alias never fired" conclusion describes the bug, not the
> design. The root cause was the unmasked category (`0x0053` = 83, rejected by the `1..21`
> guard) which left the class cache at 0 — not the caller layout.

### Live verification, both classes (2026-11-04)

Same route as above, hidden run with `MHGU_ARMOR_DEBUG=1`, driven entirely over the control
pipe (`MHGU_CONTROL`), weapon = **Petrified Hunting Horn** (equipped, `+4 = 0x00010053`), box
built by the clean save backup so both arms pieces exist: series 23 *Jaggi Vambraces* and
series 24 *Jaggi Guards*.

| Step | Observed |
|------|----------|
| hover the Guards (3/24) | detail panel shows it, no class warning; log `lookup caller=002db66c type=3 id=24 in_range=1 swapped=00000017` |
| `A` → `A` (*Change equipment?* → Yes) | committed: `memsave 0x199b21a8` → `00180003` (type 3, id 24) |
| other armour after that equip | head `00170001`, chest `00030002`, waist `00030004`, legs `00030005` — nothing dropped |
| hover the Vambraces (3/23), `A` → `A` | committed: `00180003` → `00170003` (type 3, id 23) |
| weapon word after both | `00010053` — still the Petrified Horn |

So each class now equips over the other with the horn in hand, and re-equipping the natural
class (3/23) is not disturbed by the alias (`cls == want` → no rewrite, `swapped=00000000`).

UI notes for anyone reproducing this with the control pipe:

- The confirm box renders Ⓐ/Ⓑ glyphs for **Yes/No** and the accepted input is **`A` = Yes**;
  pressing `B` cancels. (An earlier note in this project claiming `Y`/`X` was wrong — the
  glyph is a circle around a letter, which reads like `Y`/`X` at screenshot scale.)
- D-pad **right = +1 column, left = −1 column** on the equipment grid, with wrap at the row
  end; the equipment page is `1 / 10`, eight columns visible. Presses queued while the panel
  is animating are dropped, so leave ~15–30 frames between them and verify each step by
  reading the hovered item's name in the bottom-right label / detail panel.
- Reading the equipped state: the packed word lives at **`entry + 4`** (not `+0`), and the
  entries sit at `0x199b2150/0x199b217c/0x199b21a8/0x199b21d4/0x199b2200` (head → legs) for
  the observed player base. `memsave 199b2140 d0` covers all five.


---

## 7. Control-console aids used during RE

The runtime control pipe (`MHGU_CONTROL=ctl.txt`) supports:
- `peek <addr> [words]` — dump guest memory
- `poke <addr> <hexword>` — write a guest dword (used to set `+0x94` to specific weapon `type|id<<16`)
- `find <hexbytes>` — scan committed guest memory for a byte pattern

Poking weapon words into `sPlayer+0x94+4` (`0x199b2128` at the observed base) confirmed the
weapon-category table above (`0x1a0007`=GS, `0x0f000b`=LBG, `0x010053`=Horn, `0x140013`=Horn)
and that the entry keeps the equipped weapon while it is sheathed — the "83" seen for the Horn
is just the unmasked `0x53`, see section 4.

---

## 8. Open questions / cautions

- **Scope of the alias (partly open)**: the gate demonstrably covers the equip decision
  (`0x2db66c`, section 6) and the transmog/model resolvers (`f_2db0b0` / `f_2db160` /
  `f_2db210`, section 3). The stat-recalculation path resolves through `f_33f03c`, which is out
  of band, so whether a swapped piece's derived defence and skills follow the weapon class is
  still unmeasured — one status-screen A/B with `MHGU_ARMOR_DEBUG=1` settles it. If it has to
  be aliased, hook `f_33f03c`; do not widen the caller range, because every identity/name/model
  lookup goes through it too.
- **Equip restriction path (settled)**: the "can I equip this?" decision *is* carried by the
  lookup rewrite. With the category masked correctly, the in-band caller `0x2db66c` asks for
  `type=3 id=24` and receives series 23 (`swapped=00000017`), and the real equip then commits
  (section 6, "Live verification"). `armor_class_frame`'s restore is a safety net only — in the
  verified runs it never had to fire.
- **`+0x94` is the equipped weapon record (settled)**: the earlier belief that a session never
  drawing the weapon leaves the class at 0 was an artifact of the missing mask — the entry
  holds `0x00010053` (the Petrified Horn itself) whether the weapon is drawn or sheathed, so
  the category is known on the first lookup. No second source is needed; the cache in §4 is
  belt-and-braces for a transient `type == 0`.
- **Still unmeasured**: whether defence/skill values of a swapped piece follow the weapon class
  (previous bullet). Everything else in this note is confirmed live.
- **Series-id namespace (settled for arms)**: the equipment ids are *per slot* (head, chest,
  arms all use their own numbering) while `armor_pairs.h` is indexed by the **global** record
  number of `armorSeriesData.asd`. The arms case proves they coincide: id 23 resolves to the
  Blademaster *Jaggi Vambraces* and id 24 to the Gunner *Jaggi Guards* — the same two records
  that `armor_pairs.h` pairs (`pair[23]=24`, `pair[24]=23`), both defence 6 — and the live swap
  `24 -> 23` made the gate accept the real Guards. Other slots were not exercised; if a future
  piece swaps to an unrelated series, re-check this before touching the pair table.
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
   (the equip gate 0x2db66c and the model resolvers f_2db0b0/f_2db160/f_2db210 match; the
    stat code reaches the lookup through f_33f03c and therefore keeps the real row -- §3, §6)
armor_class_frame()  -> re-equip stripped pieces, fix arms box-link, clear auto-transmog
```
