# UI

The `generated/` folder is the game's own code, lifted AArch32 to C one instruction at a
time (`#include "cpu.h"` / `"neon_rt.h"`, every function `f_<addr>(Cpu *c)` named for its
guest address, `FN_ENTER(0x…)` markers, `goto L_<addr>` for branches). It carries **no
comments and no string literals**: the recompiler never emits the game's text, and there is
nothing in these files whose name mentions the HUD, a menu, or a screen. What *is* in
`generated/` that relates to the game's UI are the specific guest instructions the port
patches, and the game functions the port's runtime calls into for the UI. Everything else —
placing the HUD, drawing the overlay, the keyboard — lives in `runtime/`, not here.

The one module that holds the UI code is `generated/main/` (the `main` executable).
`generated/sdk/` is only the Nintendo SDK library: its `extern`/HLE points are libc and
compiler-rt (`memcpy`, `__aeabi_idiv`, … — see `runtime/hle/hle_symbols.txt`), with nothing
UI in them.

`tools/gen.py`'s `PATCHES` dict is authoritative for the patch sites below: each entry is
`('main', address) -> (expected word, replacement C)`, and the generated C is what you get
from applying it (`gen.py` refuses to build if the word at that address does not match, so a
different game version is caught).

---

## 1. Display and 2D UI canvas size — `f_dbca50`

`generated/main/main_218.c`, function **`f_dbca50`** (guest `0xdbca50`). This is the game's
display-setup routine: it picks the 3D render target size and the 2D UI canvas size and
writes them into the NVN buffer config. Four instructions in it were replaced:

| Site | In `f_dbca50` | Original instruction | Replaced with (gen.py PATCHES) |
| --- | --- | --- | --- |
| `0xdbcd34` | L_dbcd34 | `mov r0, #1920` (`0xE3A00D1E`) | `c->r[0] = g_render_size[0]` |
| `0xdbcd38` | L_dbcd38 | `movw r7, #1080` (`0xE3007438`) | `c->r[7] = g_render_size[1]` |
| `0xdbcd48` | L_dbcd48 | `mov r7, #1280` (`0xE3A07C05`) | `c->r[7] = g_ui_size[0]` |
| `0xdbcd54` | L_dbcd54 | `mov r7, #720` (`0xE3A07E2D`) | `c->r[7] = g_ui_size[1]` |

The first two are the docked render size, the constants resolution mods used to patch
(1920x1080). The last two are the **2D UI canvas**, the game's default 1280x720; this is
what the game's HUD is laid out against.

The patched path has its own fall-through default at `L_dbcd10` (main_218.c:6604):

```c
L_dbcd10:;
    c->r[7] = 0x2d0u;   // 720
    c->r[6] = 0x500u;   // 1280
```

So the unpatched game hard-codes a 1280x720 UI canvas here; the port replaces the two
mov-immediate sites above with `g_ui_size` so the canvas width tracks the picture's shape.

What feeds those globals is `runtime/config.c` (`config_apply_display`):

```c
g_render_size[0] = w;  g_render_size[1] = h;   // RenderResolution / window, fit to shape
g_ui_size[1]     = 720;
g_ui_size[0]     = (uint32_t)(720.0f * g_ui_anchor_aspect + 0.5f) & ~1u;
if (g_ui_size[0] < 1280) g_ui_size[0] = 1280;   // never below the game's own canvas
```

`g_ui_anchor_aspect` is `[Display] UIAnchor`: `Center` → 16:9, `21:9`, `32:9`, or `Edges` →
the picture's aspect, clamped to at most the picture's aspect. That is the whole
"give the game's 2D canvas the HUD's width" half of the ultrawide feature.

## 2. Camera projection, aspect ratio and FOV — the three screen cameras

The same `generated/main/main_218.c` feature also touches the 3D cameras, so the 3D view and
the HUD agree. The perspective-matrix builder is **`f_9c617c`** (`generated/main/main_127.c`,
guest `0x9c617c`) — its arguments are near, far, fovy, aspect (radians). Twelve call sites
load a camera's matrix through it (`f_9c617c(c)`), but only the **three** that build a camera
made for the screen are patched, and only the two instructions that read the camera's aspect
(`+0x38`) and pass its fovy:

| Function | File | Patched sites |
| --- | --- | --- |
| `f_21c358` | `generated/main/main_001.c` | `0x21c36c`, `0x21c370` |
| `f_21c3a0` | `generated/main/main_001.c` | `0x21c3b8`, `0x21c3c0` |
| `f_af4494` | `generated/main/main_158.c` | `0xaf44a8`, `0xaf44ac` |

Each pair becomes (gen.py PATCHES; the second differs only in argument order):

```c
{ extern float g_render_aspect;
  float a = LDF(c->r[1] + 0x38u);
  c->v.f[3] = a > 1.77f && a < 1.786f ? g_render_aspect : a; }        // aspect slot

{ extern float camera_fovy(float, float);
  c->v.f[2] = camera_fovy(c->v.f[2] * c->v.f[4], c->v.f[3]); }        // fovy slot
```

A camera only gets touched if it was built for 16:9 (`a > 1.77 && a < 1.786`); any other
camera keeps its own aspect and fovy. For a 16:9 screen camera, its aspect becomes
`g_render_aspect` (`config.c`: the picture's shape, at least 16:10) — that is Hor+ / the wider
3D view — and its fovy is rewritten by `camera_fovy`:

```c
float camera_fovy(float fovy, float aspect) {           // runtime/config.c
    if (aspect != g_render_aspect) return fovy;         // not a screen camera
    fovy *= g_fov_scale;                                // [Display] FOV
    if (aspect < 16.0f / 9.0f)                          // 16:9 .. 16:10: Vert+
        fovy = 2.0f * atanf(tanf(fovy * 0.5f) * (16.0f / 9.0f) / aspect);
    return fovy;
}
```

Below 16:10 the whole picture is dropped (config.c forces 16:10 with letterbox bars); between
16:10 and 16:9 the camera keeps 16:9's width and gains height (Vert+). The other nine
`f_9c617c` callers are left untouched, so equipment-box previews, guild cards, etc. keep
their own framing.

## 3. Quest-counter menu — `uUIQuestCounter`

`generated/main/main_122.c`. The game's quest-counter class is `uUIQuestCounter`; its
methods appear here as plain `f_<addr>` functions. `runtime/hle/villagehub.c` hooks three of
them (it documents the class name and the addresses in its header):

| Function | Guest | Role (villagehub.c `#define`s) |
| --- | --- | --- |
| `f_983074` | `0x983074` | `FN_UI_ROWS` — the star rows of a rank |
| `f_981e34` | `0x981e34` | `FN_UI_UPDATE` — vtable slot 9 |
| `f_981bb0` | `0x981bb0` | `FN_UI_DRAW` — vtable slot 18 |

They are fed by the selector job (`0x5b7cf8`, filter `f_5b8090` in `main_068.c`,
category build/fill/get `0x5b85d8` / `0x5bb318` / `0x5bc96c`). The port uses these to post
Village quests at the Hub / Hunters' Pub counter (`[Village Quests] VillageQuestsInHub`), so
the menu's own tables get the extra categories — a UI layout change driven from the runtime
side, not a patch to the generated code.

## 4. Pause menu and its sub-menus

The pause menu, and the sub-menus reached from it (Items, the two "Menu 1 / Menu 2" pages,
Combine/Equipment, Status, Options), are the game's own **GUI**: their layout and labels are
data — `.gui` layout files and `gmd` text in `romfs` — not code. So nothing in `generated/` is
named for them, and the port neither patches nor hooks the menu (no `gen.py` `PATCHES` entry,
no `hle_symbols.txt` point) — but the pause menu and its sub-menus are an MT Framework class
system now **address-pinned** (identification only) in `generated/main/`. The pins come from the
class tree (`tools/retool.py dti` / GOT-aware `xref str` on the class-name string pool in rodata)
and from a runtime `fnnew` trace (boot → `MHGU_QUICKHUNT=101` into a quest → press `PLUS`; the
functions first called after the learn window are the pause-menu code).

**Pause-menu classes** (tree under `cUIObject`, plus the member `sMenu` of `cSystem`): `cUIOMenu`,
`cUIOMenu::cSetupParam`, `cUIOSMenuTop`, `cUIOSMenuSub`, `cUIOSMenuSub2`, `cUIOSMenuDownTop`,
`cUIOMenuBar`, `cUIOMenuBar::cUIOMenuBarPanel`. Class registration / constructor functions (they load the
class name via GOT and call the MTDTI ctor `f_9aedcc`):

| Class | ctor function | File |
| --- | --- | --- |
| `cUIOMenu` (registers `cUIOSMenuSub`/`Sub2`/`Top` too) | `f_82d330` | `generated/main/main_105.c` |
| `cUIOSMenuDownTop` | `f_82c0e0` | `generated/main/main_105.c` |
| `cUIOMenuBar` / `cUIOMenuBarPanel` | `f_7ce4b4` | `generated/main/main_101.c` |
| `cSystem::sMenu` | `f_758694` | `generated/main/main_095.c` |

Each class's virtual methods (its vtable, reached at runtime with the control file's `peek` through the
ctor's GOT slots — the slots are relocation-filled, so they read 0 in the static image) are the
`cUIOMenu` handler chain `f_82c288`, `f_82c77c`, `f_82cb74`, `f_82ce08`, `f_82d5d0`–`f_82d70c`,
`f_82da68`–`f_82dfe0`; every GUI class shares the `cUIObject` base virtuals `f_76dc20`, `f_76dc24`,
`f_76dc88`, `f_76dd14`, `f_76dd2c`, `f_76e200`, `f_76e23c`, `f_76e2d8`, `f_76e3ac`, `f_76e4dc`. Observed at
pause by the `fnnew` trace (entered from the GUI-thread update fns `f_76de98`, `f_76dec8`, `f_76e0c0`,
`f_76e364`): `f_82bf34`, `f_82bfc0`, `f_82c078`, `f_82c474`, `f_82c4d4`, `f_82c62c`, `f_82d17c` (virtual-call
thunk: `bl f_76e4d8`, then `ldr r1,[r0,#0x64]; blx r1`), `f_82d1ac`, `f_82d298`, `f_82da7c`, `f_82dd88`,
`f_82dde4`, and the menu-bar / tab dispatchers `f_800aa4`, `f_8027f4`, `f_802828`, `f_802864`, `f_8029f0`.

**The in-hunt tab bar and the Options submenu inside it** — inside a hunt the bar with the 13 tabs
("Menu 1 / Menu 2": Item Pouch, Combine, Combo List, Quest Info, Palicoes, Hunter's Notes, Pause |
Status, Equipment, Gestures, **Options**, End via Subquest, Abandon Quest) and the per-tab handlers
are another class: `uUIQuestStartMenu` (size `0x6b0`, parent `uUIStartMenu`, both `uUIBase`-derived;
class registration `f_879c38`, which loads the class name and calls the MTDTI ctor `f_9aedcc`). Its
vtable is runtime `0x197aeb0` (the class's ctor stores vtable+8 at object+0), so the object's first
word reads `0x197aeb8` under the control file's `peek`. The Options submenu's own window is the
`cUIOOptionWindowForGame` object of §5c (its vptr reads `0x1972c88`, i.e. class vtable runtime
`0x1972c80` = image `0x1772c80`, so `f_<addr>` names in `main_102.c` are its methods):

| uUIQuestStartMenu | vtable slot | File |
| --- | --- | --- |
| tab handler: 14-way switch on the tab index at object+`0x4c` (Status = 7, Options = 10; page 2 has 6 rows, so 6 `DOWN`s return to Status) — Options branch `f_878058` | +`0x68` `f_877ee0` | `generated/main/main_108.c` |
| tab state byte: `*(*(this+0xa8) + index*8)` | +`0x90` `f_8785f0` | `generated/main/main_108.c` |
| per-tab setter (branches on `r1` = 4 / 5) | +`0xb0` `f_8786d0` | `generated/main/main_108.c` |
| selector: flat tab index = page × row count + row (`mla` of the bytes at +`0xe5`, +`0xe6`, +`0xe8`, kept at +`0x4c`), then calls the three virtuals above | `f_86f868` (called from `0x86f578`) | `generated/main/main_107.c` |

**The `‹ x / y ›` page indicator is live object data** — the bar's tail words: word
+`0xe4` = flag byte (`01`), `+0xe5` row byte, `+0xe6` rows-on-current-page byte (`07`),
+`0xe7` flag (`ff`); word +`0xe8` = LE16 current page (0-based) + LE16 **page count**
at +`0xea` (init `0x00020000`). The init virtual `f_877d0c` builds the two page GUIs
(`f_85ca84` with layout literals `0x11c8b3c`/`0x11c8460`) and the tab groups
`f_870098(bar,7,1)` + `f_8700a4(bar,2,1)` (`0xd` = the 13 flat tabs total). `fntrace` with `r0` = bar while shown attributes the
per-frame chain to virtuals +`0x24` `f_86ed24`, +`0x4c` `f_87828c`, +`0x58` `f_86eff4`,
+`0x5c` `f_877edc`, +`0x60` `f_8782ec`, +`0x64` `f_86f0a8`, +`0x7c` `f_878b64`, +`0x94`
`f_878e80` — and poking the +`0xe8` word to `0x00030001` (page 1, count 3) redraws the
header indicator as **‹ 2 / 3 ›** the next frame (`build/shots/pc_2of3.png`), count 0
hides it (`bar_page2.png`), rows `+0xe6` 7→6 collapses the page's tab rows
(`bar_rows6.png`), and page `+0xe8` 0→1 flips the bar to the second page (`bar_page2.png`
row set). So a page-count/text change needs the object's words written and the page GUIs
constructed via `f_85ca84` per page — the header number node re-derives from the words by
itself, and page/tab *labels* come from the constructed page GUI layouts.

| cUIOOptionWindowForGame | vtable slot | File |
| --- | --- | --- |
| initialize (calls the base's `f_7e5664`, clears the flags at +`0x85`/+`0x86`) | +`0x18` `f_7e6f98` | `generated/main/main_102.c` |
| per-frame update: state byte at +`0x85` (0 → base `f_7e5840`, `0xa` → the draw path) | +`0x4c` `f_7e7000` | `generated/main/main_102.c` |
| item count accessor — returns `0x1b` (27 items; the object keeps 8 items per page and 4 pages) | +`0x60` `f_7e7534` | `generated/main/main_102.c` |
| page/item setup: passes (9, `0x17`) to `f_75bc4c` | +`0x64` `f_7e753c` | `generated/main/main_102.c` |
| per-item label/layout: jump tables on (page ≤ 3, item ≤ `0x1c`) | +`0x68` `f_7e7558` | `generated/main/main_102.c` |
| per-item value metadata: jump tables on (page ≤ 3, item ≤ `0x1c`) | +`0x6c` `f_7e7d34` | `generated/main/main_102.c` |
| input: calls the base's `f_7e635c`, then a 30-entry table on the selected item | +`0x90` `f_7e87d0` | `generated/main/main_102.c` |
| "finished" stub, returns 0 | +`0x98` `f_7e9084` | `generated/main/main_102.c` |
| shared base `cUIOOptionWindowBase` (vtable runtime `0x1972b04`) versions: `f_7e5664`, `f_7e5840`, `f_7e5714` (a 6-way switch over an item-state struct), `f_7e635c`, `f_7e6978` | +`0x18`/`0x4c`/`0x8c`/`0x90`/`0x94` | `generated/main/main_102.c` |

The window's fields (a `0x90`-byte object, read with `peek`): +`0x00` vptr, +`0x30` and +`0x3c`
child GUI objects, +`0x5c` the item objects' class vptr (`0x196cac8`), +`0x68` page index, +`0x69`
item index on the page, +`0x6a` = 8 (items per page; the header draws 4 pages, `f_7e7534` returns
27 items), +`0x74` = mode (2 = changing values), +`0x85` = state. Changing **any** item runs one
shared path — no per-item function exists — observed with `fnnew 7e0000 7f0000 2500` re-armed
before each `RIGHT` press on all 8 items of page 1: `f_7e87d0` → `f_7e635c` → `f_7e753c` →
`f_7e7558` → `f_7e5714` → `f_7e7d34`. So new (port) entries plug into the item tables of
`f_7e7558` / `f_7e7d34` / `f_7e87d0` and the count `f_7e7534` returns, not into per-item code.

**Every tab of the bar and the window class it opens** — the 14-way switch inside `f_877ee0`
(jump table at image `0x877f10`) branches per tab index; three tabs are actions, not windows:

| Tab index | Branch in `f_877ee0` | Tab |
| --- | --- | --- |
| 0 | `f_877f7c` | Item Pouch |
| 1 | `f_877f8c` | Combine |
| 2 | `f_877f9c` | Combo List |
| 3 | `f_877fac` | Quest Info |
| 4 | `f_877fbc` | Palicoes |
| 5 | `f_877fe4` | Hunter's Notes |
| 6 | `f_878028` | Pause (action — closes the menu) |
| 7 | `f_878038` | Status |
| 8 | `f_878048` | Equipment |
| 9 | `f_877f5c` | Gestures |
| 10 | `f_878058` | Options |
| 11 | `f_878090` | End via Subquest (action) |
| 12 | `f_878184` | Abandon Quest (action) |
| 13 | `f_877f6c` | unused (14th row) |

Which window class is open for which index was confirmed at runtime: the control file's `find`
of the class vptr (the 4 little-endian bytes of class-table+8) locates the live heap object, and
`fntrace 200000 2347000 40 <obj> <obj+4>` (an r0 filter on that one object) then logs 40 per-frame
calls while the tab is shown. Page flipping preserves the **row**, not the cursor index: one `LEFT`
from Gestures (page 2, row 2) lands on Quest Info (page 1, row 2).

Each window class is a `0x18`-header class table in the image at `0x177xxxx` (the object's vptr =
class table + 8, e.g. `0x1970b50` for `0x1770b48`); the slots below are measured from that vptr, and
the shared `cUIObject` base virtuals (`f_76dc20`–`f_76e4dc`, `f_7635cc`–`f_7635f0`, `f_200b54`, the
pure-virtual thunks `f_200011`–`f_200018`) are omitted. Slot meanings are the same across all of them:
`+0x00`/`+0x04` ctor (complete/delegating) and dtor, `+0x08`/`+0x10` init/cleanup, `+0x14` per-frame
update, `+0x28`/`+0x44`/`+0x48`/`+0x4c` the draw/branch and input group, `+0x6c`/`+0x70` child-window
setup and the "finished" predicate.

| Tab | Window classes (class table → runtime vptr) | File |
| --- | --- | --- |
| Item Pouch | `cUIOItemPouch` `0x1770b48` → `0x1970b50`, `cUIOItemPouch::cUIODelAndItemWindow` `0x1770ae0` → `0x1970ae8` | `generated/main/main_100.c` |
| Combine | `cUIOMix` `0x1771a50` → `0x1971a58`, helper `cUIOMix::cSetupParam` `0x1771a30` | `generated/main/main_101.c` |
| Combo List | `cUIOMixList` `0x1771ae0` → `0x1971ae8` (parent `cUIOListWindow` `0x176d188`) | `generated/main/main_101.c` |
| Quest Info | `cUIOQuestInfo` `0x1774b14` → `0x1974b1c` | `generated/main/main_104.c` |
| Palicoes | `cUIOOtomo` `0x1772ef8` → `0x1972f00`, `cUIOOtomoLow` `0x1773d60` → `0x1973d68`, `cUIOOtomoStatus` `0x1773e58` → `0x1973e60` | `generated/main/main_102.c`, `main_103.c` |
| Hunter's Notes | `cUIOHunterNote` `0x1770164` → `0x197016c` plus its four live sub-windows `cUIOHunterNoteTips` `0x176ff88` → `0x196ff90`, `cUIOHunterNoteStandard` `0x17700f0` → `0x19700f8`, `cUIOHunterNoteMonsterList` `0x1770008` → `0x1970010`, `cUIOHunterNoteWeaponGuide` `0x177007c` → `0x1970084` (the tab's own rows: Control Type, Request Log, Hunter Info, Monster List, Weapon Controls, Manual) | `generated/main/main_100.c` |
| Status | `cUIOStatus` `0x1776a80` → `0x1976a88`, `cUIOStatusParamWindow` `0x1776810` → `0x1976818` | `generated/main/main_105.c` |
| Equipment | `cUIOEquipList` `0x176e3cc` → `0x196e3d4`, `cUIOStatusEquipWindow` `0x1776948` → `0x1976950` | `generated/main/main_097.c`, `main_105.c` |
| Gestures | `cUIOStatusStyleWindow` `0x17768e0` → `0x19768e8` | `generated/main/main_105.c` |
| Options | `cUIOOptionWindowBase` `0x1772b04` → `0x1972b0c`, `cUIOOptionWindowForGame` `0x1772c80` → `0x1972c88` (table above) | `generated/main/main_102.c` |

Own virtuals per window class:

- `cUIOItemPouch` — `+0x00 f_7bb070` `+0x04 f_7bb074` `+0x08 f_7bb0a4` `+0x10 f_7bb0a8` `+0x14 f_7bc63c` `+0x18 f_7bb0ac` `+0x1c f_7bb0b0` `+0x28 f_7bb0b4` `+0x44 f_7bb5a8` `+0x48 f_7bb0e8` `+0x4c f_7bb5ac`; its nested `cUIOItemPouch::cUIODelAndItemWindow` adds `+0x00 f_7ba4dc` `+0x04 f_7ba4e0` `+0x14 f_7bc62c` `+0x18 f_7ba510` `+0x28 f_7ba528` `+0x44 f_7bab18` `+0x48 f_7ba55c` `+0x4c f_7bab1c` and re-uses the eleven functions above at `+0x68`…`+0xb4` (the delete/item sub-window shares the pouch's handlers)
- `cUIOMix` — `+0x00 f_7ce84c` `+0x04 f_7ce8a0` `+0x08 f_7ce914` `+0x10 f_7ce918` `+0x14 f_7cf8dc` `+0x28 f_7ce9d8` `+0x3c f_7ce91c` `+0x44 f_7ceb80` `+0x48 f_7cea0c` `+0x4c f_7ceb84` `+0x6c f_7cf8d4` `+0x70 f_7ce654` `+0x80 f_7cf8d8` `+0x84 f_7ce6d0` (helper `cSetupParam`: `f_7cf8ec`, `f_7cf91c`)
- `cUIOMixList` — `+0x00 f_7cfb18` `+0x04 f_7cfb1c` `+0x08 f_7cfb4c` `+0x10 f_7cfb50` `+0x14 f_7d05a8` `+0x28 f_7cfb54` `+0x3c f_7c519c` `+0x44 f_7d0010` `+0x48 f_7cfb88` `+0x4c f_7d0014` `+0x50 f_7c54ec` `+0x60 f_7c5a40` `+0x64 f_764688` `+0x68 f_7c5664` `+0x6c f_7c5a54` `+0x70 f_7d04ec` `+0x80 f_7d05a4` `+0x84 f_7cfa9c`; the list base `cUIOListWindow` (shared with Combo List and Palicoes' `cUIOPetMenu`) contributes `f_764674`, `f_7640cc`
- `cUIOQuestInfo` — `+0x00 f_814608` `+0x04 f_81460c` `+0x08 f_81463c` `+0x10 f_814640` `+0x14 f_815e70` `+0x18 f_814678` `+0x28 f_814644` `+0x48 f_8146e8` `+0x6c f_815e6c` `+0x70 f_814530`
- `cUIOOtomo` — `+0x00 f_7ea7a8` `+0x04 f_7ea7ac` `+0x08 f_7ea7dc` `+0x10 f_7ea7e0` `+0x14 f_7f0e44` `+0x28 f_7ea7e4` `+0x44 f_7eaa24` `+0x48 f_7ea818` `+0x4c f_7eaa28` `+0x6c f_7f0e40` `+0x70 f_7ea6b4`; `cUIOOtomoStatus` — `+0x00 f_7fefcc` `+0x04 f_7fefd0` `+0x08 f_7ff000` `+0x10 f_7ff004` `+0x14 f_8007d4` `+0x28 f_7ff008` `+0x44 f_7ff498` `+0x48 f_7ff03c` `+0x4c f_7ff49c` `+0x6c f_8007d0` `+0x70 f_7fef4c`; `cUIOOtomoLow` — `+0x00 f_7fe3c8` `+0x04 f_7fe3cc` `+0x08 f_7fe3fc` `+0x10 f_7fe400` `+0x14 f_7fedb8` `+0x28 f_7fe7e0` `+0x44 f_7fe48c` `+0x48 f_7fe404` `+0x4c f_7fe490`
- `cUIOHunterNote` — `+0x04 f_7b1910` `+0x08 f_7aeb10` `+0x18 f_7b1914` `+0x1c f_7aec98` `+0x2c f_7b1918` `+0x30 f_7b0708` `+0x40 f_7b191c` `+0x44 f_7b0d7c` `+0x54 f_7b1920` `+0x58 f_7b15f0`; `cUIOHunterNoteTips` — `+0x00 f_7aedbc` `+0x04 f_7aee10` `+0x14 f_7b1924` `+0x18 f_7b0510` `+0x1c f_7b0614` `+0x28 f_7aeb1c` `+0x44 f_7aebf0` `+0x48 f_7aeb50` `+0x4c f_7aebf4` `+0x60 f_7b0220` `+0x64 f_7aee84` `+0x68 f_7b04b8`; `cUIOHunterNoteStandard` — `+0x00 f_7aeb18` `+0x04 f_7b1650` `+0x14 f_7b1954` `+0x18 f_7b1880` `+0x1c f_7b189c` `+0x28 f_7aeb1c` `+0x44 f_7aebf0` `+0x48 f_7b1680` `+0x4c f_7b18b0` `+0x60 f_7aec2c` `+0x64 f_7b16b8` `+0x68 f_7b1874`; `cUIOHunterNoteMonsterList` — `+0x00 f_7b0774` `+0x04 f_7b07c8` `+0x14 f_7b1934` `+0x18 f_7b0d4c` `+0x1c f_7b0d68` `+0x28 f_7aeb1c` `+0x44 f_7aebf0` `+0x48 f_7aeb50` `+0x4c f_7aebf4` `+0x60 f_7aec2c` `+0x64 f_7b083c` `+0x68 f_7b0b48`; `cUIOHunterNoteWeaponGuide` — `+0x00 f_7b0ea8` `+0x04 f_7b0efc` `+0x14 f_7b1944` `+0x18 f_7b134c` `+0x1c f_7b1454` `+0x28 f_7aeb1c` `+0x44 f_7aebf0` `+0x48 f_7aeb50` `+0x4c f_7aebf4` `+0x60 f_7b155c` `+0x64 f_7b0f70` `+0x68 f_7b1294`
- `cUIOStatus` — `+0x00 f_8328c0` `+0x04 f_8328f0` `+0x08 f_832940` `+0x10 f_832944` `+0x14 f_833798` `+0x18 f_83297c` `+0x1c f_83299c` `+0x28 f_832948` `+0x44 f_832b60` `+0x48 f_8329d4` `+0x4c f_832b64`; `cUIOStatusParamWindow` — `+0x00 f_82e804` `+0x04 f_82e808` `+0x14 f_833738` `+0x18 f_82e8f4` `+0x1c f_82eaa0` `+0x28 f_82e838` `+0x44 f_82e8f0` `+0x48 f_82e86c` `+0x4c f_82eae0`; the sibling `cUIOStatusUserWindow` (`0x1776768`) uses `f_82e184`, `f_82e188`, `f_833728`, `f_82e234`, `f_82e770`, `f_82e1b8`, `f_82e774`, `f_82e1ec`, `f_82e778`
- `cUIOEquipList` — `+0x00 f_789a78` `+0x04 f_789af4` `+0x14 f_789f00` `+0x28 f_789b90` `+0x44 f_789c48` `+0x48 f_789bc4` `+0x4c f_789c4c` `+0x6c f_789efc` `+0x70 f_7899dc`; `cUIOStatusEquipWindow` — `+0x00 f_830114` `+0x04 f_830158` `+0x14 f_833768` `+0x28 f_8301bc` `+0x44 f_83023c` `+0x48 f_8301f0` `+0x4c f_830240`
- `cUIOStatusStyleWindow` — `+0x00 f_82f8e0` `+0x04 f_82f8e4` `+0x14 f_833758` `+0x18 f_82f914` `+0x1c f_82f934` `+0x28 f_82f954` `+0x44 f_82fa24` `+0x48 f_82f988` `+0x4c f_82fb44` (the Hub-only `cUIOStyleMenu` `0x177700c` — `f_837684`, `f_837688`, `f_838140`, `f_83813c`, `f_839178`, `f_839140`, `f_8376b8`, `f_8376bc`, `f_837eb0`, `f_839174`, `f_8375dc` — is never instantiated in a hunt, and neither is `cUIOPetMenu` `0x1774520` nor `cUIOHunterNoteLow` `0x17706a4`: those are the town/room GUI)
- the bar itself: `cUIOMenuBar` `0x17719a0` → `0x19719a8` (`+0x00 f_7cd64c` `+0x04 f_7cd650` `+0x08 f_7cd680` `+0x10 f_7cd684` `+0x14 f_7ce4a4` `+0x28 f_7cd688` `+0x44 f_7cd910` `+0x48 f_7cd6bc` `+0x4c f_7cd914` `+0x6c f_7ce48c` `+0x70 f_7cc4f8` `+0x80 f_7ce490` `+0x84 f_7cd5b8`) and its panel `cUIOMenuBar::cUIOMenuBarPanel` `0x1771900` → `0x1971908`, whose virtuals are the tab dispatchers already listed (`f_802244`, `f_8027f4`, `f_802828`, `f_802864`, `f_8029f0`, plus `f_7cc578`, `f_7cc57c`, `f_7cc5ac`, `f_7cc5e0`, `f_7cc64c`)

Every tab's window shares the same call shape: the GUI-thread update calls the window's `+0x14` and
`+0x40` virtuals each frame (`f_76e2d8` and `f_76e4d8`, the `cUIObject` base implementations), so a
value/label change goes through that class's own `+0x44`/`+0x48`/`+0x4c` handlers — the Options window
is the only one of these whose item tables were also mapped in detail (above). None of this is hooked:
the map exists so a future port feature can address the right class rather than guess.

**The village (town) pause bar and its sub-menus** — pressing `PLUS` in the village (the hub) opens the
same bar: `cUIOMenuBar` `0x17719a0` and `cUIOMenuBarPanel` `0x1771908` are live there too. Its `uUI`-level
owner is the town class `uUILobbyStartMenu` (size `0x800`, parent `uUIStartMenu`; class table image `0x17307ec`
→ runtime `0x19307ec`, object vptr `0x19307f4`), registered by `f_21095c` (`main_000.c`) together with the whole
village UI family (`uUIItemBox`, `uUIKitchen`, `uUIPostOffice`, `uUIQuestBoard`, `uUIPetMenu`, `uUIRoomService`,
`uUIBarTopMenu`, `uUIJukeBox`, `uUICrossCoin`, the `uLobbyNpc…` NPCs — each class table is derived from that
driver's GOT pairs, e.g. `uUIItemBox` `0x1730558`, `uUIQuestBoard` `0x173079c`, `uUIRoomService` `0x17307d8`);
that table only holds the class's dtor thunk `f_2108f4` and construct thunk `f_20ef90` (`f_20ef90` news `0x800`
bytes and tail-calls the ctor), i.e. the town classes carry no per-tab virtuals and the tab logic is the same
free-function family the hunt uses. The village bar has **8 + 8** rows — page 1: Item Pouch, Combine, Combo
List, Quest Info (grey), Palicoes (grey), Hunter's Notes, Friends, Chat; page 2: Wireless Settings, Status,
Equipment, Guild Cards, View Special Permits, Gestures, Options, Quit Game — and the page flip again keeps the
**row** (`RIGHT` from Combine lands on Status, `RIGHT` from Hunter's Notes lands on Gestures). Three rows are
actions, not windows: Wireless Settings leaves the menu and enables the multiplayer shortcuts (ZL Multiplayer
Menu, X World Map, Y Hunter Search), View Special Permits prints "See how many Special Permits you own." and
closes the bar, Quit Game quits. Every other row's window was located the same way as in the hunt (`find` of
class-table+8 then `fntrace 200000 2347000 120 <obj> <obj+4>` on the live object, 120 per-frame calls) and
screenshotted while open:

| Village tab | Window classes live while the tab is shown (class table → runtime vptr) | File |
| --- | --- | --- |
| Item Pouch | `cUIOItemPouch` `0x1770b48` → `0x1970b50`, `cUIOItemPouch::cUIODelAndItemWindow` `0x1770ae0` → `0x1970ae8`, `cUIOItemExpWindow` `0x1770a64` → `0x1970a6c` | `generated/main/main_100.c` |
| Combine | `cUIOMix` `0x1771a50` → `0x1971a58` | `generated/main/main_101.c` |
| Combo List | `cUIOMixList` `0x1771ae0` → `0x1971ae8` | `generated/main/main_101.c` |
| Hunter's Notes | `cUIOHunterNoteTips` `0x176ff88` → `0x196ff90` plus `cUIOHunterNoteStandard` `0x17700f0` → `0x19700f8`, `cUIOHunterNoteMonsterList` `0x1770008` → `0x1970010`, `cUIOHunterNoteWeaponGuide` `0x177007c` → `0x1970084`; its own rows are Control Type, Request Log, Hunter Info, Monster List, Weapon Controls, Manual (`cUIOHunterNote` `0x1770164` itself is **not** instantiated in the village) | `generated/main/main_100.c` |
| Friends | `cUIONetworkFriendList` `0x17727a8` → `0x19727b0` (rows Friend List, Friend Registration, Ignore List, Add to Ignore List) | `generated/main/main_102.c` |
| Chat | `cUIOKaiwaExpWindow` `0x1777bb8` → `0x1977bc0` (rows Chat Menu, Shoutout Reset); the chat panel itself is `cUIOChatWindow` `0x176db94` → `0x196db9c` with `cUIOChatFixedPharase` `0x176db04` → `0x196db0c` | `generated/main/main_106.c`, `main_096.c` |
| Status | `cUIOStatus` `0x1776a80` → `0x1976a88`, `cUIOStatusParamWindow` `0x1776810` → `0x1976818`, `cUIOStatusUserWindow` `0x1776768` → `0x1976770` | `generated/main/main_105.c` |
| Equipment | `cUIOEquipList` `0x176e3cc` → `0x196e3d4`, `cUIOStatusEquipWindow` `0x1776948` → `0x1976950` | `generated/main/main_097.c`, `main_105.c` |
| Guild Cards | opens its own list window `cUIOGuildCardList` `0x176faac` → `0x196fab4` (rows Check, Edit, Send, Card List), whose rows open `cUIOGuildCardView` `0x176fbdc` → `0x196fbe4` and `cUIOGuildCardEdit` `0x176f9b4` → `0x196f9bc` | `generated/main/main_097.c`, `main_098.c` |
| Gestures | `cUIOStatusStyleWindow` `0x17768e0` → `0x19768e8` (Greet, Bow, Nod, Shrug, Wave, Clap, Fist Pump, Point over 2 pages) | `generated/main/main_105.c` |
| Options | `cUIOOptionWindowForGame` `0x1772c80` → `0x1972c88` — the **same** window and the same 27-item / 4-page tables as the in-hunt submenu above | `generated/main/main_102.c` |

The Options row is not a guess: with the village submenu open, only `cUIOOptionWindowForGame`'s own virtuals
run per frame (`+0x4c f_7e7000`, `f_7e5840`, `+0x94 f_7e6978`, `+0x98 f_7e9084`), and moving the item cursor
re-arms the trace to the identical chain `f_7e87d0` → `f_7e635c` → `f_7e753c` → `f_7e7558` → `f_7e5714` →
`f_7e7d34`. `cUIOOptionWindowForChat` `0x1772bc0` → `0x1972bc8` and `cUIOOptionWindowForNetwork` `0x1772d40`
→ `0x1972d48` objects also exist in the village but only their base draw `f_76e2d8` runs; `cUIOOptionWindowBase`
`0x1772b04` → `0x1972b0c` is never instantiated (it is the shared base).

Own virtuals of the village-only window classes (`cUIOItemExpWindow` — `+0x00 f_7b9f94` `+0x04 f_7b9f98`
`+0x08 f_7b9fc8` `+0x10 f_7b9fcc` `+0x14 f_7ba310` `+0x28 f_7b9fd0` `+0x44 f_7ba0ac` `+0x48 f_7ba004`
`+0x4c f_7ba0b0` `+0x6c f_7ba30c` `+0x70 f_7b9f30`; `cUIOGuildCardList` — `+0x00 f_79b0b8` `+0x04 f_79b0bc`
`+0x08 f_79d7b8` `+0x10 f_79d7bc` `+0x14 f_79e744` `+0x28 f_79d7c0` `+0x48 f_79b0ec` `+0x4c f_79b974`
`+0x6c f_79e6fc` `+0x70 f_79afc8` `+0x80 f_79e700` `+0x84 f_79e644`; `cUIOGuildCardView` — `+0x00 f_79eaa0`
`+0x04 f_79ec64` `+0x08 f_7a16d8` `+0x10 f_7a16dc` `+0x14 f_7aa434` `+0x28 f_7a16e0` `+0x48 f_79ec94`
`+0x4c f_79faf8` `+0x6c f_7aa3ec` `+0x70 f_79e8d4` `+0x80 f_7aa3f0` `+0x84 f_7aa2f8`, then the same tab-panel
group the bar panel uses at `+0x90`…`+0x124` (`f_80217c`, `f_802244`, `f_802248`, `f_8027f4`, `f_802828`,
`f_80259c`, `f_80224c`, `f_8026b8`, `f_8025b8`, `f_802844`, `f_802864`, `f_802878`, `f_8028b8`, `f_8028f0`,
`f_802910`, `f_802928`, `f_802948`, `f_80294c`, `f_76c780`, `f_80298c`, `f_76c78c`, `f_76c794`, `f_8029f0`);
`cUIOGuildCardEdit` — `+0x00 f_797f54` `+0x04 f_797fd4` `+0x08 f_79977c` `+0x10 f_799780` `+0x14 f_79abb8`
`+0x28 f_799784` `+0x48 f_798074` `+0x4c f_7984ac` `+0x6c f_79abb4` `+0x70 f_797e7c` `+0x7c f_79ad5c`
`+0x80 f_79ad60` `+0x84 f_79ad90` `+0x8c f_79ad94` `+0x90 f_79ae94` `+0xa4 f_79ad98` `+0xc0 f_79ae48`
`+0xc4 f_79adcc` `+0xc8 f_79ae4c` `+0xe8 f_79ae90` `+0xec f_79acfc`; `cUIOKaiwaExpWindow` — `+0x00 f_8427f8`
`+0x04 f_843098` `+0x08 f_8430c8` `+0x10 f_8430cc` `+0x14 f_843370` `+0x28 f_8430d0` `+0x44 f_84303c`
`+0x48 f_84297c` `+0x4c f_843040` `+0x60 f_843104` `+0x64 f_8431bc` `+0x68 f_843294` `+0x6c f_843350`
`+0x7c f_843358` `+0x80 f_8426e0` `+0x90 f_84335c` `+0x94 f_84304c`, whose input path is the one seen open in
the village (`+0x60 f_843104`, `+0x68 f_843294`, `+0x6c f_843350`, `f_8429a4`); `cUIOChatWindow` — `+0x00 f_7774c4`
`+0x04 f_7774c8` `+0x14 f_777cd8` `+0x18 f_777548` `+0x28 f_7775f8` `+0x48 f_77762c` `+0x4c f_77766c`
`+0x6c f_777cd4` `+0x70 f_777460`; `cUIOChatFixedPharase` — `+0x00 f_775d5c` `+0x04 f_775d60` `+0x14 f_7772d0`
`+0x28 f_77651c` `+0x48 f_776550` `+0x4c f_7769d4` `+0x6c f_777288` `+0x70 f_775b98` `+0x80 f_77728c`
`+0x84 f_775cc8`; `cUIONetworkFriendList` — `+0x00 f_7da180` `+0x04 f_7da184` `+0x14 f_7db244` `+0x18 f_7da3a8`
`+0x28 f_7da3c0` `+0x48 f_7da3f4` `+0x4c f_7da78c` `+0x6c f_7db240` `+0x70 f_7da0f8`, and its siblings
`cUIONetworkAreaSelect` `0x177272c` → `0x1972734`, `cUIONetworkOnlineSelect` `0x1772824` → `0x197282c`,
`cUIONetworkRoomList` `0x17728a0` → `0x19728a8`, `cUIONetworkRoomSetting` `0x177291c` → `0x1972924` (the
multiplayer menu Wireless Settings opens) have the same nine-slot shape.

Reproducing the village pass: `MHGU_HIDDEN=1` only (no `MHGU_QUICKHUNT`), boot to the title, `A` ×8 to load the
save and reach the village, `PLUS`. `A` on an action row or on a greyed row closes the whole menu, so a probe run
must skip those rows and park the cursor by `DOWN`s only — a run that walked straight through mis-attributed
Gestures/Friends/Options/Chat to the town. Options is reached without touching Wireless Settings: `PLUS`,
`RIGHT`, `DOWN` ×6, `A`.

**The house's Item Box in the village (the Home)** — the house, entered from the village
destination menu with `X`, `A`, `A`, is one GUI set: `mhgu/romfs/nativeNX/eng/GUI/04_lobby/home/`
holds `item_box/menu_style.gui` (the box's tab bar), `item_box/menu_style_down.gui`,
`item_box/myset_equip.gui`, `item_box/skill.gui`, `item_box/stone.gui` — the loadout chest's
Style / My Set / Skill / Stone tabs — plus `delivery_item.gui` (the Delivery Box) and
`gallery.gui`, `gallery_down.gui`, `gallery_down_list.gui` (the Gallery), the Home's two other
services. The box's stored state is a system object: `sItemBox` (parent `cSystem`, size
`0x72e4`, dti `0x1858418`) with class table image `0x173f5dc` → runtime `0x193f5dc`, object
vptr `0x193f5e4`, 190 function entries; its ctor `f_39b394` (`main_024.c`) registers the name
through `f_9aedcc` at `0x39b49c` and stores that class table (`xref 0x173f5dc` lands inside it),
and `sPastItemBox` (size `0x1b4c`) keeps the previous hunt's loadout. The UI object itself is
the town family's `uUIItemBox` (size `0x1190`, dti `0x184d030`; class table image `0x1730558`
→ runtime `0x1930558`, object vptr `0x1930560`, ctor `f_20e804` (`main_000.c`) at slot `+8` of
that table — the same dtor-thunk/construct-thunk-only shape as `uUILobbyStartMenu`, so again
no per-tab virtuals), registered by the same driver `f_21095c`.

The box's windows, one class table per window (class table → runtime object vptr; ctor at
slot `+0x00`), attributed to tabs by the GUI layout names in `item_box/`:

| Item Box window | class table → vptr | ctor | File |
|---|---|---|---|
| tab bar `cUIOStyleMenu` | `0x177700c` → `0x1977014` | `f_837684` | `main_105.c` |
| Style detail `cUIOStyleMenuDown` | `0x1777088` → `0x1977090` | `f_8393c0` | `main_105.c` |
| My Set `cUIOMySetMenuLow` / `cUIOMySetStyleInfo` | `0x1772634` → `0x197263c` / `0x17726b0` → `0x19726b8` | `f_7d8f3c` / `f_7d9644` | `main_102.c` |
| My Set name `cUIOMySetStyleName` | `0x176e518` → `0x196e520` | `f_78a36c` | `main_097.c` |
| Skill `cUIOSkillWindow` / `cUIOSkillDetailWindow` | `0x17769b0` → `0x19769b8` / `0x1776a18` → `0x1976a20` | `f_8305e8` / `f_830ff4` | `main_105.c` |
| Skill list / slot `cUIOSkillList` / `cUIOSkillSlot` | `0x1776214` → `0x197621c` / `0x17761a4` → `0x19761ac` | `f_82972c` / `f_82b148` | `main_105.c` |
| Stone `cUIOStone` / `cUIOStoneList` / `cUIOStoneSlot` | `0x1776ddc` → `0x1976de4` / `0x1776d70` → `0x1976d78` / `0x1776d04` → `0x1976d0c` | `f_835350` / `f_835a70` / `f_835354` | `main_105.c` |
| stored items `cUIOItemBox` / `cUIOItemExpWindow` | `0x1770a50` → `0x1970a58` / `0x1770a64` → `0x1970a6c` | `f_7b9e04` / `f_7b9f30` | `main_100.c` |

`cUIOItemBox` is the one of these whose class table is taken from its own ctor: `f_7b9e04`
registers the class name and stores `+8` of `0x1770a50` (`GOT` rt `0x1a3f83c`), where the
dti-slot GOT pairing does not land. The Home's two NPC services are pinned the same way:
Gallery `cUIOGalleryList` `0x176f658` → `0x196f660`, `cUIOGalleryListLow` `0x176f704` →
`0x196f70c`, `cUIOGalleryLow` `0x176f780` → `0x196f788` (ctor `f_794398`, `main_097.c`), and
Delivery Box `cUIODeliveryBox` `0x176e14c` → `0x196e154`, `cUIODeliveryBoxAnytime` `0x176e1c8`
→ `0x196e1d0`, `cUIODeliveryBoxAnySub` `0x176e230` → `0x196e238` (ctor `f_77c6e8`, `main_096.c`).

The box is now runtime-confirmed open. From the house's teleport spawn, holding `STICK_UP` for
4 s (120 presented frames at this port's 30 fps present — the tens-of-frames attempts stopped
~2 m short of the marker post) reaches the chest's prompt, and an idle `A` tap at that stance
opens the Item Box menu: Store Items / Take Items / Item Sets / Combine-Manage Items / Change
Appearance / Manage Equipment / Equipment Sets / Change Style/Arts. (Timed writes that press `A`
*while still walking* do not open it — the tap must land after arriving.) At the open stance the
pinned classes report live in `find` (heap slots vary per run): `cUIOStyleMenu` `0x1977014`
(tab bar), `cUIOStyleMenuDown`, `cUIOMySetMenuLow`, `cUIOMySetStyleInfo`, `cUIOMySetStyleName`,
`cUIOSkillSlot`, `cUIOStoneList`, `cUIOStoneSlot`; the `cUIOItemExpWindow` +
`cUIOSkillWindow`/`cUIOSkillDetailWindow` trio is live everywhere and is not evidence. Re-armed
`fntrace` filtered on each object's `r0` shows a shared per-frame base virtual `f_76e2d8` (120×
while a window is shown), and each window's own virtuals first-ran within the pinned ctor's
region: StyleMenu `f_8376b8`–`f_837b28`, StyleMenuDown `f_8393f4`/`f_8398c0`, MySetMenuLow
`f_7d8f70`/`f_7d8f74`/`f_7d935c`/`f_7d93e4`, MySetStyleInfo `f_7d9678`/`f_7d96f0`/`f_7d9914`/
`f_7d9928`, MySetStyleName `f_78a3a8`/`f_78a3dc`/`f_78a430`/`f_78a4f0`, SkillSlot
`f_82b184`/`f_82b1ec`/`f_82b684`/`f_82b79c`, StoneList `f_835aac`/`f_835b4c`, StoneSlot
`f_835398`/`f_8353d8`/`f_835ef8`/`f_835f48` — so the class-table pins above stand runtime-
verified. The same tap's `fnnew` also starts the Store/Take-Items pouch window
`cUIOItemPouch::cUIODelAndItemWindow` (`f_7ba510`+, drawn as the "Item Pouch 2/4" page window
with X Sort / Y Quantity rows) and shared components `cUIOPageCounter`,
`cUIOLeftRightArrows`, `cUIOPanelBase`, `cUIOColorPanel`, `cUIOAppearanceList`,
`cUIOOtomoEquipColor`. Screenshots: `build/shots/x_t3.png` (the menu), `tab1`–`tab5.png` (sub-
windows as the selection moves). Chamberlyne (the Wycademy housekeeper, a fixed NPC at the
marker post) only takes the `A` when the tap lands within her own radius — from ~2 m short she
hijacks it into dialog, from the full-walk stance the box takes it.

### The whole Item Box menu, toured row by row

The full press script for all eight rows runs from the open stance with ~8-frame presses
(the menu reacts instantly); the tour harness first sends a `B` after the teleport (an
open shortcut panel hijacks the opening `A` into **World Map** — observed), then walks
with two forward bursts (`STICK_UP 150` + `STICK_UP 40`) so the hunter is pressed against
the chest. Rows 1–3 run as scripted; from row 4 the extra exit presses get consumed inside
the sub-menu, so the bar cursor drifts a row or two off the intended one — but every
window family still opens and every row's `fnnew` phase (re-armed per row, so each window's
`+0x4` *show* virtual re-fires on every reopen) attributes cleanly. Newly pinned classes
(class table image → runtime vptr = table + 8 + BASE; slot offsets are from that vptr):

| Sub-menu row(s) | Window classes (table → vptr runtime) |
| --- | --- |
| Store / Take Items | `cUIOItemPouch::cUIODelAndItemWindow` `0x1770ae0` → `0x1970ae8` (pouch page window; +0x4 show `f_7ba4e0`, +0x18 `f_7ba510`, +0x28 `f_7ba528`, +0x44 `f_7bab18`, +0x80 `f_7bb0ac`, +0x84 `f_7bb0b0`, +0xb4 `f_7bb5ac`, +0xf8 `f_7ba454`, +0x10c `f_7bafe4`), `cUIOKaiwaExpWindow` `0x1777bb8` → `0x1977bc0` (Item Points page, +0x60 `f_843104`…+0x6c `f_843350`), `cBoxCursor` `0x176cb94` → `0x196cb9c` (+0x24 `f_757e00`), `cNumSelecter` `0x17729b8` → `0x19729c0` (Qty selector), `cUIOPageCounter::cSetupParam` `0x1773ed4` → `0x1973edc` |
| Item Sets | `cUIOMySetMenuLow` `0x1772634` → `0x197263c` (set list + slot detail — two-panel layout, `s_row3.png`) |
| Combine / Manage Items | `cUIOActionList` `0x176d110` → `0x196d118` (organize/sell actions), `cUIOFormItemWindow` `0x176cf6c` → `0x196cf74` (recipe/form list, +0x18 `f_76dc28`, +0x54 `f_76e4dc`), `cUIOLeftMenu` `0x1771180` → `0x1971188` |
| Change Appearance | purchase panel: `cUIOMix` `0x1771a50` → `0x1971a58`, `cUIOMixList` `0x1771ae0` → `0x1971ae8` (buy-list), `cUIOMoney` `0x1771b70` → `0x1971b78`, `cUIODLCItemPack` `0x176e2c0` → `0x196e2c8`, `cUIOFacilityShopArms` `0x176e580` → `0x196e588`; appearance: `cUIOAppearanceList` `0x176d630` → `0x196d638`, `cUIOAppearanceColor` `0x176d5ac` → `0x196d5b4`, `cUIOAmulet` `0x176d530` → `0x196d538`, `cUIOInsectCustom` `0x17709c0` → `0x19709c8` (Bugjailed), `cUIOOtomoEquipColor` `0x1773834` → `0x197383c` (Palico colour) |
| Manage Equipment | `cUIOStatusEquipWindow` `0x1776948` → `0x1976950`, `cUIOStatusParamWindow` `0x1776810` → `0x1976818` + the pinned Stone/Skill/Style/MySet families' construction virtuals (+0x28/+0x44/+0x48) |
| Change Style / Hunter Arts | `cStyleArtsChange` `0x176c87c` → `0x196c884` (first slot +0x0 `f_748a08`) with `cUIOSkillDeco`/`cUIOSkillCompare` construction |

Shared component classes used by every sub-window: `cUIOPanelBase::cSetupParam`
`0x17740c8` → `0x19740d0`, `cUIOListWindow::cSetupParam` `0x176f6d0` → `0x196f6d8`,
`cUIOArrangePanel::cSetupParam` `0x176deb8` → `0x196dec0`,
`cUIOLeftRightArrows::cUIOLeftRightArrow` `0x177120c` → `0x1971214`, plus the shared
`cInsertCursor`/`cCursor` bar cursors. Row 5's closing `B×3` fully exits the box (no
box-family heap instances remain at that probe; `s_row5.png` shows only the shortcut
panel); `r0`-filtered `fntrace` on every new heap instance again shows the shared
per-frame base virtual `f_76e2d8` at 120 hits. Screenshots: `build/shots/walked.png`,
`s_row1.png`–`s_row8.png` (per row), `done.png`.

The one place the pause menu *is* handled concretely is the port's HUD repositioning for
non-16:9 pictures, in `runtime/nvn/vulkan/exec.c` (the vertex stage of 2D draws, cbuf 3). The
whole pause menu and every sub-menu inside it are drawn at NVN layer `pos_z = 97`; the text and
labels that sit over them — e.g. the **Qty.** label of the quantity box — are a separate layer,
`98` (exec.c ~lines 2265–2266). The rules there (exec.c ~2263–2287):

| Part of the pause menu | Screen region (1280×720 layout) | Anchor |
| --- | --- | --- |
| Left window — Quest Info | `x ≤ 430` | Left |
| Submenu & Pause Menu content — Combine, Items, the "Menu 1 / Menu 2" pages | `x > 430` | Right |
| Quest Complete banner's lower border (slides in from y 739) | `y > 650` | Bottom (else Top) |

So the menu's panel (the two menu pages, the Combine / Items sub-windows) stays pinned to the
right edge and grows into the empty middle, while a left-side panel (Quest Info) stays on the
left — the split is at `x = 430`. Vertically the menu keeps its **Top** anchor; only the Quest
Complete banner's lower border drops to the bottom edge. Layer 98 (the Qty. label and any text
over a menu) follows the same rule as the layer-97 menu under it; before this fix it used the
generic rule, which left it centered 36 px below its top-anchored box (exec.c:2266).

The pause menu also affects a neighbouring layer, `100`. When a pass drew a layer-97 menu, the
flag `ui_menu_pass` is set (exec.c:693 declaration, cleared at 1133, set at 2276). A layer-100
left window then — the **Request Log** info panel / quest card (`x < 630`, `y < 560`) — is
top-anchored so it lines up with the top-anchored 97 menu beside it, or the window splits.
Without a menu in the pass (the results screen, whose labels on the left and numbers on the right
are one table) those windows keep the layer's default Bottom anchor (exec.c:2286).

Sub-menu scissor: the item / Combine sub-windows are drawn through a scissor sized for the
original layout; when a moved bar slides out from under it, exec.c relaxes that scissor for the
one draw (`ui_relax_scissor`, set exec.c:2298 at the layer-103 item selector wheel, applied at
2738/2740) so the moved bar is not cut off.

Do **not** confuse this with the quest-counter menu in section 3: `uUIQuestCounter`
(`main_122.c`) is the village / Hub quest board's own rank and star menus (`villagehub.c`:
"rank menu (UI state 1)" and "star menu (2)"), a separate GUI from the in-hunt pause menu.

The pause menu is exercised by the ultrawide feature (`docs/STATUS.md` item 9): "tested...
1680x720 Center, 1792x504 / 1680x840 Edges: hunt HUD, pause menu, help box, equipment windows".
One related bug is still open — a white flashing bar seen in pause menu > Status, "not found yet:
need a screenshot" (`docs/STATUS.md:100`).

---

## 5. Complete inventory of the game's UI

Every UI element the game draws is either the game's own GUI (code in `generated/main/` driven
by `.gui`/`gmd` **data** in `romfs`), or the port's own overlay. The game never draws HUD text
from `generated/`: the labels are data. What the port can *do* to each element is reposition it
for non-16:9 pictures, and that repositioning is keyed to the element's NVN **layer** — the
`pos_z` in the 2D draw's matrix (`exec.c` cbuf 3, matrix slot `m[14]`). That layer is how the
whole HUD/menu set is enumerated below (exec.c ~2215–2325 plus the model-preview path ~1210–1260
and the viewport/scissor ~2720–2745).

### 5a. HUD and in-hunt overlays — one anchor per layer

`is_fullscreen` fades and full backdrops (centered quad, `m[0]≥1200` or `m[5]≥700`) are
**left alone** (exec.c:2217–2221); only `!is_fullscreen` draws are moved. Layers, and which port
flag each sets:

| `pos_z` layer | UI element | Horizontal | Vertical | Flag |
| --- | --- | --- | --- | --- |
| `< 95` (94.0) | central notifications & banners ("Time limit: 50 min.") | Center | — | — |
| `96.0` | dialogue & help text box (bottom left) | Left | Bottom | — |
| `97.0` | pause menu & sub-menus: **Menu 1/2 pages, Items, Combine** (`x>430`) Right; **Quest Info** left window (`x≤430`) Left | by `x` split @430 | Top (Quest Complete border `y>650` → Bottom) | sets `ui_menu_pass` |
| `98.0` | text drawn over menus — **Qty. label of the quantity box** — follows the 97 menu under it | as 97 | as 97 | — |
| `99.0` | results-screen rule between its rows (`y` 266) | Center | forced **Bottom** (no center band) | — |
| `100.0` | **Hunter Arts HUD** & bottom-right prompts (`y>600`); **target-cam selector** (`x>1000, y<360`); **Request Log** info panel / quest card (`x<630, y<560`); centered windows (`x` 640) stay one piece | Right | Bottom; **Top** for the selector, cutscene top bar (`y<10`), and the left panel when `ui_menu_pass` | `ui_menu_pass` |
| `103.0` | in-hunt **Status HUD** (clock, HP bar, stamina, gauges, **palico** status, `y<500`) Left/Top; **Item Selector wheel** (`y≥500`) Right/Bottom | by `y` split @500 | Top / Bottom | sets `ui_relax_scissor` on the item bar |
| `105.0` | **minimap** & **target cam** | Right | Top | — |
| `110.0` | loading-screen scrolling backdrop (strips `y` 168/433/570) — also lock-on markers & other effects mid-hunt; strips keep height, nothing moves vertically | Center | — | sets `ui_loading_pass` on the strips |
| else | general fallback | `x≤430` Left, `x≥650` Right, else Center | `y<240` Top, `y>480` Bottom, else Center | — |

Loading-screen special case: once `ui_loading_pass` is set, every layer `94 ≤ pos_z < 109.5`
scales about the **center** instead of by edge, so the quest panel's headers (100), text (101)
and title (105) stay one picture (exec.c:2250–2255). Layer 110's mid-hunt lock-on markers must
**not** be treated as loading strips — only `y` 168/433/570 set the flag (exec.c:2234).

### 5b. 3D models drawn inside a HUD window

`is_3d_overlay` is `pos_z > 180` (exec.c:2242). The Smithy's **weapon / armor preview** and the
**equipment-box screens** show a 3D model through a viewport + scissor the game sized for its
1280×720 canvas; when the window moves and squeezes, the model would spill past its frame. `ui_preview_map`
(exec.c:1219–1244) maps that viewport/scissor with the same anchor rule — horizontally by where
its centre lands (`cx ≤430` Left, `≥650` Right, else Center; split @430/650, `pm->kx = s`),
vertically it follows how the **last HUD quad** (the window frame drawn just before) was anchored,
`ui_last_vanchor` 0 Top / 1 Centre / 2 Bottom (exec.c:694, 2322). The viewport keeps its size and
only its centre moves; only the scissor follows exactly (exec.c:2723–2740). Returns false for the
scene itself (a viewport spanning ~the whole picture) or a plain 16:9 frame.

### 5c. HUD text and menu labels — data, not code

All UI strings (button prompts, menu labels, quest text, the Options-menu items) are `.gui`
layout files and `gmd` text under `romfs`; no `generated/` file holds them. The in-game **Options
menu** is that same GUI (.gui layouts, gmd text, per-item code): adding a port option means
extending its item tables / text / handlers — the effort judgement is still open (`docs/STATUS.md`
item 8), but its code is now pinned. The Options window is the GUI class family `cUIOOptionWindowBase`
(ctor `f_7e69e4`, `generated/main/main_102.c`), with per-mode variants `cUIOOptionWindowForChat`
(ctor `f_7e6dc8`), `cUIOOptionWindowForGame` — the single-player Options screen, opened both from the title's main menu and
from the Options tab of the in-hunt pause menu (ctor `f_7e90a8`) — and
`cUIOOptionWindowForNetwork` (ctor `f_7e9674`). Their virtual methods (vtables read at runtime via
`peek`) are the handler chains `f_7e55e0`–`f_7e6fcc` (Base: `f_7e5630`, `f_7e5664`, `f_7e6978`–`f_7e69dc`,
`f_7e6ba0`–`f_7e6fcc`), `f_7e6efc`–`f_7e9664` (ForGame: `f_7e9230`, `f_7e9264`, `f_7e928c`, `f_7e92e4`–`f_7e9664`),
`f_7e7000`–`f_7e87d0` (ForChat) and `f_7e9660`–`f_7ea578` (ForNetwork). No `gen.py` `PATCHES` entry and no
HLE hook exists for them yet. Observed live (`MHGU_HIDDEN=1`; boot → title, `PLUS` → main menu, navigate):
`fntrace 7e4000-7ec000` logs the window's virtuals as the Options screen opens and runs — `f_7e4870`,
`f_7e48e0`, `f_7e49a0`, `f_7e49d4`, `f_7e4a20`, `f_7e55e0`, `f_7e56e0`, `f_7e57b4`, `f_7e6b18`, `f_7e6ba4`,
`f_7e6efc`, `f_7e6fcc`, `f_7e91dc`, `f_7e928c`, `f_7ea104`, `f_7ea4d8`, `f_7eaa24`, … — entered from the
class-dispatch sites `f_85c200`, `f_85c264`, `f_85c2b0`, `f_85c578` (they load the object's vtable slot and
call it), the `cUIObject` base-virtual call sites `f_76dfe4`/`f_76e260`, update/draw callers `f_801be8`/`f_801c30`,
and the menu-creation fn `f_386824`. The **in-hunt** submenu is pinned the same way (`fnnew 200000
2347000 40000` learning through boot → town → hunt, then `PLUS`, `LEFT` or `RIGHT` to the bar's page 2
— the cursor lands on Status (tab index 7) — `DOWN`×3 to Options (index 10; page 2 has 6 tabs, so a
6-`DOWN` cycle returns to Status), `A`; `peek` of the two objects gives vptrs `0x197aeb8` =
`uUIQuestStartMenu` and `0x1972c88` = `cUIOOptionWindowForGame`), and a `fnnew 7e0000 7f0000 2500` window re-armed before `RIGHT` on each
of the 8 items of page 1 (Music Volume, SFX Volume, HUD, Map, Camera Angle, Camera Controls, Camera
Speed, Scope Controls) logs the same six functions every time — see §4 for the slot tables.

**Stage B findings (custom "Graphics" tab label and FXAA submenu — not yet working):**

- Table `.gmd` files (e.g. `table/StartMenuMsg_eng.gmd`) are uncompressed and hold only label
  names, a hash block and the display string pool (`[hdr][name table][0x400 hash][names][pool]`,
  551 labels, 556 strings). They contain no glyph records, so a new string cannot be added by
  editing the file alone.
- At runtime the parsed table is `rGUIMessage` (class table `0x1794e3c`, object size `0x4a0`,
  `f_d84114` entry lookup) with its own count and key arrays at `+0x88`/`+0x98` and a 12-byte
  entry array at `+0xa0`. Its glyph records are 36 bytes (`f_d84be4`, stride `0x24`).
- The pause bar's label path is a separate decoded table: rows of stride `0x90` in
  `generated/main/main_300.c` (`f_15813fc`, `f_15a4224`), with the text analyzer at
  `cGUIMessageAnalyzer` (size `0x84`). Row labels are baked glyph runs, not inline strings.
- The 12-byte records at `LD32(bar+0xc0) + 12*flat` are visibility parameters: `+6` a flag, `+8`
  a festa param id. Changing `+8` does not change the label text. The third page shows no label
  for flat 14 even when `+8` is set to the Options id.
- Searching RAM for the StartMenuMsg table by its `Menu`/`Item` ASCII failed: the strings are
  not stored inline in the rows. Neighbouring help-text tables were found instead.
- A depth-first walk from the bar (`[walk]` debug, reverted) finds several objects with the
  count word 551/556 at `+0x3a`. The count-556 objects (e.g. `tbl 1ae80004`) hold binary
  node-like rows, not label text, so they are not the StartMenuMsg label store. The walk
  results also change between runs (heap addresses shift), so pointer-based lookup is fragile.
- Most likely label store: `rGUIMessage` (`+0xa0` entries, 36-byte glyph records). Next step:
  find its instance from the class singleton and dump the entry for "Options" to confirm the
  record format, then copy G, r, a, p, h, i, c, s glyph records into a synthesized "Graphics" entry.
- `rGUIMessage` instance search: a `find` for its vtable word `0x1994e44` (class table `0x1794e3c`
  + 8) returns exactly one hit, at `0x023262e8`. That object is the class descriptor (its name
  pointer is image `0x177a923`, its fields are small integers), not an entry-holding instance. So
  no live `rGUIMessage` instance exists at pause-open time; the bar's labels are not drawn from it.
- Bar child class map (pause open, `MHGU_GFXDBG` debug, reverted): the 34 children
  `[LD32(bar+0x34)][s]` have vtables that resolve to panels/gauges/windows, with slot 2 the page
  panel (vptr `0x1976488`). The panel's `+0x90` head/tail list does not contain vtable-bearing
  nodes at `+0x8` (sibling guess wrong), so the text-node children are not found yet.
- Paging init: `f_87006c(bar, rows, _, totalTabs)` takes the tab total in **`r3`** (stock 13).
  It runs inside the bar's init virtual, so raising it there builds the third page during init
  (`runtime/hle/pausepages.c`, `paging_init`). The third page then renders (3/3), still unlabelled.
- GUI instance child layout (from `f_7455c4` / `f_76e528`): `inst = LD32(gui+0x24)`, child count
  `inst+0x298`, child pointer array `inst+0xf8` (overflow array `inst+0x294`). The page panel's
  43 children have vtables `0x198f4d8` and `0x198f5b8` (GUI node classes). This replaces the
  earlier guesswork about the panel's `+0x90` list.
- **Labels are baked glyph runs, not strings.** Overwriting every RAM copy of "Item"/"Pouc"
  (the romfs mapping copies, 32 hits) does not change any visible label, and no "Item Pouch"
  ASCII exists outside the romfs mapping. So a synthesized "Graphics" label requires writing
  glyph quads into a label node, which needs the quad/atlas format.
- **Custom Graphics submenu (working):** `runtime/hle/pausepages.c` turns the real
  `cUIOOptionWindowForGame` into a one-row menu. `f_7e7534` (item-count virtual, stock 27) reports
  1 while `MHGU_PAUSE_GRAPHICS` is on; the label virtual `f_7e7558` redirects row 0 to item 2 (the
  stock On/Off row); the value virtual `f_7e7d34` answers from `g_config.fxaa`; the festa setter
  `f_85e5bc` is intercepted for param `0xb6` (its twin `0xb7` is swallowed) and flips the flag.
  `fxaa_skipped()` in `runtime/nvn/vulkan/exec.c` reads `g_config.fxaa` every frame, so the toggle
  takes effect immediately.
- Option row text: rows are ASCII strings in a table loaded with the GUI data (e.g. `HUD\0On\0Off\0Map\0`)
  and resolved at draw time, but a row's label and its On/Off value strings share one fixed-width
  slot, so a 3-character row like HUD cannot be renamed to a 4-character word. Renaming the row to
  "FXAA" (and the window title to "Graphics") needs a new text node, not a string edit.
- Not yet done: the synthesized "Graphics"/"FXAA" labels.

### 5d. Menus that are full GUI systems, not HUD layers

- **Quest-counter / quest-board menu** — class `uUIQuestCounter`, methods as `f_<addr>` in
  `generated/main/main_122.c` (`f_983074` rank star-rows, `f_981e34` vtable 9, `f_981bb0` vtable 18);
  fed by the selector job (`0x5b7cf8`, filter `f_5b8090` `main_068.c`, categories `0x5b85d8`/`0x5bb318`/`0x5bc96c`).
  The counter lays out its own **rank menu** (UI state 1) and **star menu** (2). The port posts
  Village quests at the Hub / Hunters' Pub counter via `runtime/hle/villagehub.c`.
- **Pause menu & its sub-menus** — see §4: class `cUIOMenu` (ctor `f_82d330`, `main_105.c`) with
  `cUIOSMenuSub`/`Sub2`/`Top`, `cUIOSMenuDownTop` (ctor `f_82c0e0`); the tab bar `cUIOMenuBar` /
  `cUIOMenuBarPanel` (ctor `f_7ce4b4`, `main_101.c`); update/handler chain `f_82c288`–`f_82dfe0`
  plus tab dispatchers `f_800aa4`, `f_8027f4`. Its `.gui` layouts are `romfs/nativeNX/eng/GUI/02_common/menu/`
  (`menu`, `menu_option`, `menu_status`, `menu_concoct`, `menu_itemPouch`, `menu_equipMenu`, …).
- **Options window** (the Options tab of the pause menu, and its chat/network variants) — see §5c and
  §4: `cUIOOptionWindowBase` (ctor `f_7e69e4`), `cUIOOptionWindowForGame` (ctor `f_7e90a8`, `main_102.c`),
  `cUIOOptionWindowForChat` (`f_7e6dc8`), `cUIOOptionWindowForNetwork` (`f_7e9674`); in a hunt the tab
  opens it through `uUIQuestStartMenu` (`main_107.c`/`main_108.c`: tab switch `f_877ee0`, Options branch
  `f_878058`, selector `f_86f868`) and its value changes run `f_7e87d0` → `f_7e635c` → `f_7e753c` →
  `f_7e7558` → `f_7e5714` → `f_7e7d34`; layout `GUI/02_common/menu/menu_option`.
- **Nameplates / nameplates over heads** are part of the HUD and are exercised by the ultrawide
  fix (`docs/STATUS.md` item 9 lists "nameplates" tested at 1680x840 Edges).

### 5e. The port's own UI (never in `generated/`)

- **Inline software keyboard / hunter-name entry.** `nn::swkbd` is a Switch applet, absent from
  both modules; the port supplies it by HLE in `runtime/hle/swkbd.c` (+ `runtime/textinput.c`)
  — inline-keyboard callbacks the game registers on its own thread (character creation).
- **Forge / ImGui overlay** (`runtime/forge.c`, `runtime/forge_imgui.c`, `runtime/overlay.cpp`,
  `tools/imgui_thunks.py`) — the port's debug/settings panel, unrelated to the generated game code.
- **SDL host window** (`runtime/ui.c`) — the port's window/swapchain setup, not game UI.

### 5f. FSR and the HUD boundary

When FSR is on, the scene is rebuilt at `g_fsr_output` and the **HUD is drawn at output size on
top** (`runtime/nvn/vulkan/fsr.c:3`). "The start of the HUD" is detected as the first draw into
the target that is not a post pass (`fsr.c:11`, `hud_start()` at `fsr.c:1553`, exec.c:1091/2598):
from there on, the HUD renders at full resolution. The repositioning rules of 5a/5b are what keep
it intact at that boundary.

## Quick reference

| UI feature | In `generated/` | Fed / used by |
| --- | --- | --- |
| 3D render size | `f_dbca50` @ `0xdbcd34`/`0xdbcd38` → `g_render_size` | `config.c` (`RenderResolution`) |
| 2D UI canvas | `f_dbca50` @ `0xdbcd48`/`0xdbcd54` → `g_ui_size` | `config.c` (`UIAnchor`); placed by exec.c |
| Aspect / FOV | `f_21c358`, `f_21c3a0`, `f_af4494` → `g_render_aspect`, `camera_fovy` | `config.c` (`AspectRatio`, `FOV`) |
| Perspective matrix | `f_9c617c` (`main_127.c`) | 12 callers; 3 patched |
| Quest-counter menu | `f_983074`, `f_981e34`, `f_981bb0` (`main_122.c`) | `runtime/hle/villagehub.c` |
| Pause menu & sub-menus | `cUIOMenu` ctor `f_82d330` (`main_105.c`), `cUIOMenuBar` ctor `f_7ce4b4` (`main_101.c`); handlers `f_82c288`–`f_82dfe0`, tab dispatch `f_800aa4`/`f_8027f4`; in-hunt tabs `uUIQuestStartMenu` `f_877ee0` (`main_108.c`) and one window class per tab (`cUIOMix`, `cUIOMixList`, `cUIOQuestInfo`, `cUIOOtomo*`, `cUIOHunterNote*`, `cUIOStatus*`, `cUIOEquipList`, `cUIOOptionWindow*`) — §4 tables, not hooked | game GUI; placed by `exec.c` (layers 97/98/100) |
| Item Box (village house) + Delivery Box / Gallery | `uUIItemBox` ctor `f_20e804`, `sItemBox` ctor `f_39b394`; tab windows `cUIOStyleMenu` `f_837684`, `cUIOMySetMenuLow` `f_7d8f3c`, `cUIOSkillWindow` `f_8305e8`, `cUIOStone` `f_835350`, items `cUIOItemBox` `f_7b9e04`, Gallery ctor `f_794398`, Delivery ctor `f_77c6e8` — §4 tables, static (never opened at runtime) | game GUI (`04_lobby/home/`) |
| HUD elements (status, item wheel, minimap, target cam, Hunter Arts, banners, help box, nameplates) | game GUI code (per-layer, not address-pinned) | repositioned by `exec.c` cbuf 3, by `pos_z` layer |
| Smithy / equipment 3D previews | `is_3d_overlay` (pos_z > 180) | `exec.c` `ui_preview_map` viewport/scissor |
| UI text / labels | — | `.gui` + `gmd` in `romfs` |
| Options menu (title, in-hunt pause submenu and the village pause submenu) | `cUIOOptionWindowForGame` ctor `f_7e90a8`, handlers `f_7e6efc`–`f_7e9664`, pause-submenu path `f_7e87d0`/`f_7e7558`/`f_7e7d34` (`main_102.c`), pause tabs `uUIQuestStartMenu` `f_877ee0`/`f_8785f0`/`f_8786d0` (`main_108.c`) — pinned, not hooked | open task: adding port entries — `docs/STATUS.md` item 8 |
| Village pause menu & sub-menus | bar owner `uUILobbyStartMenu` class table `0x17307ec` (registered with the town UI family by `f_21095c`, `main_000.c`); one window class per tab (`cUIOItemPouch`, `cUIOMix`, `cUIOMixList`, `cUIOHunterNote*`, `cUIONetworkFriendList`, `cUIOKaiwaExpWindow`, `cUIOStatus*`, `cUIOEquipList`, `cUIOGuildCard*`, `cUIOStatusStyleWindow`, Options = `cUIOOptionWindowForGame`) — §4 tables, not hooked | game GUI; placed by `exec.c` (layers 97/98) |
| Software keyboard / name entry | — | `runtime/hle/swkbd.c` + `runtime/textinput.c` |
| Forge / ImGui overlay | — | `runtime/forge*.c`, `runtime/overlay.cpp` |
| FSR HUD boundary | — | `runtime/nvn/vulkan/fsr.c` (`hud_start`) |
