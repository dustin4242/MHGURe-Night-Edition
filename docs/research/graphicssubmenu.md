# The in-game Graphics submenu

This is a detailed account of how the `Graphics` entry and its submenu were built, how the
entry is slipped into the real Options window, how each row works, and what to do to add
another row. It is the working note behind `docs/UI.md` §5c; the code lives in
`runtime/hle/pausepages.c`.

Short version: the submenu is not a new window. It is the stock Options window
(`cUIOOptionWindowForGame`) made to report six items instead of 27, with its row/value
virtuals overridden so the six rows draw our labels and values, its pager shrunk to one
page, and all input for the borrowed option masked. The borrowed option's string block is
patched in place so the rows show our values.

Everything is runtime HLE — no romfs, exefs or game-code edits.

- Always enabled (no environment switch); `MHGU_PAUSE_PAGES` still grows the pause bar if set.
- The existing rows: `Display` (a nested submenu with Aspect Ratio, Resolution, Framerate and
  VSync), `FXAA`, `MSAA`, `FSR`, `FSR Quality`, `FSR Sharpness`.
- Build: `make -j4 FFMPEG=system`, binary `build/mhgurecomp`.

---

## 1. The stock Options window

The in-hunt Options window is `cUIOOptionWindowForGame`. Its object vptr at runtime is
`0x1972c88`. The parts we depend on:

### 1.1 The pager

The window embeds a paging/setup param at `obj + 0x4c`. The fields we read and write:

| Field | Offset | Size | Meaning |
| --- | --- | --- | --- |
| row | `obj + 0x59` (`param + 0x0d`) | u8 | highlighted row within the page |
| rows/page | `obj + 0x5a` (`param + 0x0e`) | u8 | rows drawn per page |
| page | `obj + 0x5c` (`param + 0x10`) | u16 | current page (0-based) |
| page count | `obj + 0x5e` (`param + 0x12`) | u16 | number of pages |
| total items | `obj + 0x60` (`param + 0x14`) | u16 | total rows across all pages |
| edit state | `obj + 0x74` | u32 | `2` while a value editor is open |

`obj + 0x60` is the **total item count**, not the last index. It has to be restored when
leaving the submenu, otherwise the engine recomputes the pages and the extra row vanishes.

The stock list has 27 items at 8 rows/page: 4 pages, last page 3 rows. The Graphics entry is
item 27 (page 3, row 3) — see §2.

### 1.2 The virtuals we hook

The row drawing is driven by four virtuals on the window (listed with the module the
recompiler put them in, `generated/main/main_102.c`):

| Vtable | Address | Signature (guest registers) | Role |
| --- | --- | --- | --- |
| `+0x60` | `0x7e7534` | `(obj) -> count` | total item count |
| `+0x68` | `0x7e7558` | `(obj, out, page, row) -> void` | fills the row description |
| `+0x6c` | `0x7e7d34` | `(obj, page, row) -> value` | current value index |
| update | `0x7e5840` | per-frame window update | where we re-run layout |
| layout | `0x7e635c` | `(obj) -> void` | rebuilds the visible row nodes |

`f_7e7534` returns the count. `f_7e7558` receives `(page, row within the page)`, not an
absolute item index — the absolute index is `page * rows_per_page + row`. `out` is a
caller-stack struct of roughly `0x4c` bytes (only the fields below are relevant); the fields
we touch:

| out offset | Meaning |
| --- | --- |
| `+0x00` | label string pointer (the caller copies it into the label node) |
| `+0x04` | row type/format id |
| `+0x0a` | min value (u16) |
| `+0x0c` | max value (u16) |
| `+0x10`, `+0x14`, … | optional explicit value-string pointers (only some items set these) |

`f_7e7d34` is `(obj, page, row)` and returns the value index; our hook uses `r2` as the row.

The per-frame update `f_7e5840` is hooked so that after a value change we can re-run
`f_7e635c` and rebuild the row nodes (see §4.4).

The value setters run through `f_85e5bc` (the "festa parameter" setter in this module). We
swallow it while the submenu is up (§4.6).

### 1.3 How a row is built

`f_7e635c` calls the count/row/value machinery and then rebuilds text nodes. Two properties
matter:

1. **The value text is read from the game's string block when the row is built.** It is not
   read from the `out` label pointer we replace. This was established experimentally: with
   only the host buffer changed, the row still showed the stock strings (`Type 1`); with the
   borrowed string block patched, it showed our strings.
2. **A text node whose string pointer did not change is not redrawn.** Changing the bytes in
   place alone leaves the old drawing on screen, because the node still points at the same
   address. The row has to be rebuilt with a different pointer.

Both facts drive the design: patch the borrowed block *during* the row build, and alternate
the string pointer between two alternating slots so the window notices the change.

---

## 2. Adding the `Graphics` row to the Options list

No new window and no new page: the entry is the **last** row of the existing list.

### 2.1 Count and position

`opt_count` overrides `f_7e7534` to return `STOCK_ITEMS + 1` (28) while the list is shown.
With 8 rows/page the engine grows the last page to four rows, so item 27 is valid.

**It must be last.** The game identifies each option by its list index in hardcoded code
(its `f_7e7d34`/setter switches are index-based). Inserting a row anywhere before the end
would make row *N* edit option *N+1*. Keeping the entry at the end leaves every stock index
and value untouched.

The same reason makes A on this row dangerous: index 27 is "Reset All Settings" in the stock
switch. The press is therefore intercepted before the engine sees it (§2.3).

### 2.2 Drawing the row

`opt_row` (the `+0x68` hook) computes the absolute item:

```c
static uint32_t row_abs(uint32_t w, uint32_t page, uint32_t row)
{
    uint32_t rows = LD8(w + 0x5au);
    return page * (rows ? rows : 1u) + row;
}
```

When the absolute item is `GRAPHICS_ITEM` (27), it swaps the row's item argument to
`PLAIN_ITEM` (2 = "HUD", a plain On/Off row) so the engine lays out a normal one-line row,
and then:

- replaces `out[0]` (the label pointer) with a host buffer containing `"Graphics"`;
- blanks the *value-0 string* of the HUD slot (`src + 4`) for the duration of that one row
  build, and restores it at the start of the next `opt_row` call. The HUD slot's "On" string
  is only hidden while the Graphics row is built.

`opt_value` also returns `0` for the Graphics row so it has no value text.

### 2.3 Entering, leaving, and input

The input hook (`pausepages_input`, called from `runtime/hle/input.c`) runs after the
timestamps in `input.c` are updated (`g_a_pressed_ns`, `g_b_pressed_ns`, `g_lr_pressed_ns`,
etc.), so masking a button bit does not lose the press for our own logic.

While the stock list is shown:

- **B / Plus**: drop the window handle (`g_opt_obj = 0`) and let the engine close the list.
  Dropping the handle prevents the bar's own A presses from being mistaken for the extra row.
- **A on the Graphics row**: clear the A bit (`*buttons &= ~(1ull << BTN_A)`) and call
  `enter_menu()`. The engine never sees the press, so "Reset All Settings" never runs.
- **A elsewhere**: left alone (stock behaviour).

The window handle is captured lazily in `opt_row` (the first row build after opening). On
that first sighting the A/B/Plus timestamps are rebased, so the press that opened the Options
window is not treated as input.

---

## 3. Entering the submenu

Entering is a pager rewrite of the same window. Nothing is allocated in the engine, and no
new class is instantiated.

```c
static void enter_menu(void)
{
    g_gfx_menu = 1;
    uint32_t w = g_opt_obj;
    if (w) {
        g_saved_rows  = LD8(PW_ROWS(w));
        g_saved_pages = LD16(w + 0x5e);
        g_saved_total = LD16(w + 0x60);
        g_saved_page  = LD16(PW_PAGE(w));
        g_saved_row   = LD8(PW_ROW(w));
        ST8 (PW_ROW(w), 0);
        ST16(PW_PAGE(w), 0);
        ST8 (PW_ROWS(w), GFX_ROWS);   // 5
        ST16(w + 0x5e, 1);            // one page
        ST16(w + 0x60, GFX_ROWS);     // six items
        ST32(w + 0x74, 0);            // make sure no editor state leaks in
    }
    g_lr_last = g_lr_pressed_ns;      // re-base input
    g_b_last  = g_b_pressed_ns;
    g_plus_last = g_plus_pressed_ns;
    g_refresh = 1;                    // force a layout
}
```

`leave_menu()` restores the saved shape (rows/page, page count, total, page, row), clears
`+0x74` again, restores the borrowed string block, and re-bases every input timestamp so
presses made inside the submenu are not seen as list input. It also sets `g_refresh`.

`g_refresh` is consumed in `opt_update`:

```c
static void opt_update(Cpu *c)
{
    fnhook_call_original(c, FN_OPT_UPDATE);
    if (g_refresh && g_opt_obj) {
        g_refresh = 0;
        c->r[0] = g_opt_obj;
        GuestFn layout = dispatch_lookup(FN_OPT_LAYOUT);
        if (layout) layout(c);
    }
}
```

This re-runs `f_7e635c` immediately, which rebuilds the rows from the current config (the
same pass that draws a normal value change).

The bar's init hook (`bar_init`, `0x877d0c`) calls `leave_menu()` if the pause menu is being
rebuilt, so the list is never left in the shrunken state.

---

## 4. How the rows are drawn and changed

### 4.1 Row override

While `g_gfx_menu` is set, `opt_row` forces every row's item argument to `VALUE_ITEM` (4 =
"Camera Angle"):

```c
if (g_gfx_menu) {
    sub_row = (int)item;      // 0 Display, 1 FXAA, 2 MSAA, 3 FSR, 4 FSR Quality, 5 FSR Sharpness
    c->r[3] = VALUE_ITEM;     // let the engine lay out a stock multi-value row
}
```

`item` (the third argument) is the row within the page, which for the one-page submenu is
the row number 0..5.

### 4.2 Labels

Each row has its own label buffer, allocated from HLE guest memory with `hle_alloc`:

```c
g_lbl[row][0] = hle_alloc(64);
g_lbl[row][1] = hle_alloc(64);
```

After the stock row virtual has filled `out`, we overwrite `out[0]` with one of the two
buffers. Two buffers are used because of the "pointer must change" rule: `g_lbl_cur` is
flipped after each value change, so the next layout sees a different label pointer and
redraws the row.

The buffers must stay exactly 64 bytes or larger; `ensure_bufs()` allocates them once, looping
over `GFX_ROWS` — it needs no change when a row is appended.

### 4.3 Values: borrowing the Camera Angle slot

The six rows do not have their own option records; they borrow the string block of item 4,
"Camera Angle". Its block starts with the 13-byte label and then five value strings in
7-byte slots at `+13, +20, +27, +34, +41` (the stock values are `Type 1`…`Type 5`). The
engine reads the value text from this block at row-build time.

The first row build in the submenu captures the block pointer (`g_tbl = LD32(out)`) and
saves 64 bytes of it. `restore_tbl()` puts those 64 bytes back when the submenu is left.

Because the text is read at build time, `fill_row(row)` is called *inside* `opt_row`, after
the stock row virtual has run and before the caller builds the value node, and rewrites the
slots with the current row's texts. Each row therefore has its own list on the same block:

```c
static const char *const g_row_texts[GFX_ROWS][5] = {
    {"Off", "On"},                 // FXAA
    {"Off", "2x", "4x", "8x"},     // MSAA
    {"Off", "FSR 3", "FSR 4"},     // FSR
    /* FSR Quality, FSR Sharpness and Framerate are special-cased, see 4.5 */
};
```

Important: the *host label buffer* is not the value source. Writing value strings into the
label buffer has no effect on the value text and can corrupt the label (it will run into the
label's NUL and show `FXAAOff`). Only the borrowed block matters.

### 4.4 The value index and redraw

`opt_value` returns the row's current index (the switch uses `c->r[2]`, the row argument):

```c
switch (c->r[2]) {
case ROW_MSAA:  c->r[0] = msaa_index(g_config.msaa); break;
case ROW_FSR:   c->r[0] = fsr_sel(); break;
case ROW_FSRQ:  c->r[0] = g_fsrq_disp; break;
case ROW_SHARP: c->r[0] = g_sharp_disp; break;
default:        c->r[0] = g_config.fxaa ? 1u : 0u; break;
}
```

The returned index selects which of the five borrowed strings the row shows. That is why
each multi-value row needs at most five values.

After a change the hook flips `g_lbl_cur` and sets `g_refresh`; `opt_update` re-runs the
layout, the label pointer differs, and the row is redrawn with the new index.

### 4.5 Rows with more than five values, or longer strings

The borrowed block has exactly five 7-byte slots, and the row description's max value is 4
(`out+0x0c`). Trying to raise the max (e.g. to 9) does **not** give ten string values: the
window falls back to number formatting / other slots, which is what an early experiment
showed (the editor displayed a bare `9` and the list showed a neighbouring item's text).
So a row can expose at most five values, and each string must fit six characters plus NUL.

Two rows need more than that. Both use the same trick: write only the *currently selected*
text, into two slots far enough apart not to overlap, and report a value index that
alternates between those two slots. The pointer change is what redraws the row.

- **FSR Quality** (5 values, names up to `UltraPerf` = 9 chars): write the current label to
  slots 0 and 4 (`+13` and `+41`, 28 bytes apart), report `g_fsrq_disp` = 0 or 4.
- **FSR Sharpness** (10 values): write the current `0.1`..`1.0` text to slots 0 and 1
  (`+13` and `+20`), report `g_sharp_disp` = 0 or 1.
- **Framerate** (8 values) and **VSync** (2 values, in the Display view): current text in slots
  0 and 1, alternating the reported index.
- **Aspect Ratio / Resolution** (6 values each, in the nested view): the current ratio name or
  height goes into slots 0 and 1, alternating the reported index.

Because only the current text has to be correct, overlapping the other slots does not
matter; the rest of the block is restored on exit.

The `out+0x0c` max is left at the stock 4 for every row, so the editor's range stays valid.

### 4.6 Input

All submenu input is handled in `pausepages_input`, using the raw press timestamps from
`input.c`:

| Input | In the submenu | Why |
| --- | --- | --- |
| B | masked, `leave_menu()` | B should return to the Options list, not close the pause menu |
| Plus | `leave_menu()` | closes the pause menu by the stock path |
| Left/Right | masked, own change | see below |
| Up/Down | passed through | moves the stock row cursor; we read it from the pager |
| A | passed through, except on `Resolution` | opens the stock value editor; the Resolution row is masked and enters the nested view |
| B (nested view) | masked, `leave_disp_menu()` | returns to the six-row Graphics view |

The left/right case deserves its own note. The row/value virtuals only change *drawing*; the
window's edit path still targets the stock item under its own cursor. Since the submenu
starts at page 0 / row 0, that item is **Music Volume**. Without masking, changing FXAA also
moved the music volume. The fix is to consume the press like B while still using it for our
own row:

```c
int dir = (*buttons & (1ull << BTN_RIGHT)) ? 1
        : (*buttons & (1ull << BTN_LEFT))  ? -1 : 0;
*buttons &= ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT));
if (g_lr_pressed_ns != g_lr_last) { ... apply to the row ... }
```

The direction is computed *before* masking. The editor still draws its `◀ value ▶` arrows and
the value updates because the value virtual and the forced layout supply the text.

A confirm on the editor would call the stock setter for the borrowed item; `param_set`
(`0x85e5bc`) returns without calling the original while `g_gfx_menu` is set, so the borrowed
option is never applied. Camera Angle does not change either.

The B that leaves the menu is held back from the engine until it is released
(`g_swallow_b`), so the same press does not immediately close the Options list.

---

## 5. The rows

All INI writes go through `config_save_key(key, value)` in `runtime/config.c`; it rewrites
the value of an existing line in place (the key must already exist in the INI).

### Display — row 0 (nested submenu)

- The row has no value (its value string is blanked in `fill_row`); A on it is masked and opens
  a second view instead of the stock editor.
- The nested view is the same window shrunk again: item count 4, rows/page 4, one page, rows
  `Aspect Ratio`, `Resolution`, `Framerate` and `VSync`. `enter_disp_menu()` saves the *Graphics*
  pager shape (rows, page count, total, page, row) in `g_disp_saved_*`; `leave_disp_menu()`
  restores it. The Options-list shape is already saved in `g_saved_*`, so the stack is two levels
  deep.
- Aspect Ratio: `4:3, 3:2, 16:10, 16:9, 21:9, 32:9`, cycled with left/right.
- Resolution: a typed height. A on it starts a four-digit entry (`textinput_begin(NULL, 4)` with
  `textinput_digits_only(true)`), the row shows the digits plus a blinking `|` (500 ms), Enter
  applies (240..4320), Escape or B cancels, A also confirms. The entry is shown through the same
  two-slot method, using `g_num_disp` as the reported index and forcing a relayout on every
  keystroke/blink. Left/right does nothing on this row.
- Framerate: `30, 40, 60, 72, 120, 144, 165, 240`, shown as the actual `g_config.fps` (which may
  be a rate outside the eight; the first step snaps to the nearest). Applying sets
  `g_config.fps`, calls `vi_set_framerate()` (`runtime/hle/vi.c`) and writes `FPS`. That function
  writes the target into the engine's limiter (`+0x3c` of the main system object at
  `0x2326764`); the timing thread re-reads it each tick, so it applies live.
- VSync: `Off/On`; toggling calls `vk_set_vsync()` (`runtime/nvn/vulkan/device.c`), which sets
  `g_config.vsync` and raises a flag. The next frame (render thread, under the Vulkan lock)
  rebuilds the swapchain, because the present mode is chosen at creation; the log shows
  `[vk] swapchain ...`. It applies live. Live: yes for both.
- Either aspect/height change calls `res_apply(aspect, height)`, which computes
  `width = round(height * aspect)` (rounded up to even), then writes both
  `Resolution = WxH` and `AspectRatio = <name>` to the INI and updates `g_config`. The aspect
  is read back from `g_config.aspect_ratio` (or the window's shape when it is Auto) and the
  height from `g_config.height`, so the rows always reflect the INI; a height outside the six
  choices is displayed as-is.
- Live: no for the size itself — the window, swapchain and game render targets are created at
  start-up (the same class of limit as FSR quality). The INI is authoritative for the next
  launch and the log says so.
- Typing details: while the entry is active the game receives no input (`pausepages_input` clears
  the button word), and the physical keyboard types into `runtime/textinput.c`; gamepad B cancels,
  A confirms, Plus cancels and leaves.
- Note the list area keeps the full Options-window height: with only four rows the lower slots
  are empty. The window's own height is not driven by the pager, so this is cosmetic.

### FXAA — row 1

- Values: `Off`, `On` (indices 0, 1).
- State: `g_config.fxaa`.
- Live: yes. `runtime/nvn/vulkan/exec.c`: `fxaa_skipped()` reads `g_config.fxaa` every
  frame, so the change is visible immediately.
- INI: `FXAA = On/Off`.

### MSAA — row 2

- Values: `Off`, `2x`, `4x`, `8x` (indices 0..3).
- Mapping: `msaa_index(v)` / `msaa_value(i)` convert between the sampled count and the index.
- Apply: `g_config.msaa = ...` and `vk_set_msaa(...)` (the same path as the F7 key; FSR keeps
  MSAA off, so this is best treated as a non-FSR setting). The render pass reads `g_vk.msaa`
  every frame, so the new sample count is used from the next pass.
- Live: yes.
- INI: `MSAA = Off/2x/4x/8x`.

### FSR — row 3

- Values: `Off`, `FSR 3`, `FSR 4` (indices 0..2).
- Mapping: Off → `FSR = Off, Upscaler = FSR3`; FSR 3 → `FSR = On, Upscaler = FSR3`;
  FSR 4 → `FSR = On, Upscaler = FSR4`.
- Apply: `fsr_apply()` sets `g_config` and writes both INI keys.
- Live: only partly. The upscaler, its device features and the render resolution are built at
  start-up. Off stops the upscaler immediately (`g_fsr_on = false`). Turning it back on works
  only if the upscaler built at boot is the same choice; enabling from a non-FSR boot or
  changing 3 ↔ 4 logs `takes effect on the next start`.

### FSR Quality — row 4

- Values: `NativeAA`, `Quality`, `Balanced`, `Perf`, `UltraPerf` (indices 0..4).
- INI: full names `NativeAA / Quality / Balanced / Performance / UltraPerformance`.
- Display: the short list above, written via the two-slot trick (§4.5), because the names do
  not fit the 7-byte slots.
- Apply: `fsrq_apply()` sets `g_config.fsr_quality` and writes the INI.
- Live: no. The quality chooses the render scale, which is fixed when the game's render
  targets are created at start-up. The INI is authoritative for the next launch.

### FSR Sharpness — row 5

- Values: `0.1` … `1.0` in 0.1 steps (ten steps, indices 0..9).
- Display: current text in slots 0 and 1, alternating the reported index (§4.5).
- Apply: `g_config.fsr_sharpness = (index + 1) * 0.1f`, then `Sharpness = x.y` in the INI.
- Live: yes. The FSR 4 provider and the FSR 3 dispatch read `g_config.fsr_sharpness` every
  frame (`runtime/nvn/vulkan/fsr.c`: `.sharpness`, `di.sharpness`,
  `di.enableSharpening`), so the value applies while FSR is running.

---

## 6. Adding another row

1. **Pick the state and the INI key.** The key must already exist in `mhgurecomp.ini`
   (`config_save_key` only rewrites existing lines). Add a parser entry in
   `runtime/config.c` if it is new, plus a `Config` field in `runtime/config.h`.
2. **Add the row constant and bump the count** in `runtime/hle/pausepages.c`:
   ```c
   #define ROW_MYROW  5
   #define GFX_ROWS   6u
   ```
   `ensure_bufs()` loops over `GFX_ROWS`, so it picks up the new row automatically.
3. **Add the name** to `g_row_names`, and the value texts to `g_row_texts` if the row has at
   most five values, each at most six characters:
   ```c
   {"Off", "Low", "High", NULL, NULL},
   ```
   Rows with more than five values or longer strings must be special-cased in `fill_row`
   with the two-slot trick (§4.5) and get `{NULL, ...}` in `g_row_texts`.
4. **Return the current index** in `opt_value`'s switch:
   ```c
   case ROW_MYROW: c->r[0] = my_index(); break;
   ```
   For a two-slot row return the alternating display index instead of the real one.
5. **Add the left/right case** in `pausepages_input`:
   ```c
   case ROW_MYROW: {
       int i = (my_index() + (dir > 0 ? 1 : N - 1)) % N;  // N = number of values
       my_apply(i);       // set g_config, call the live setter, config_save_key(...)
       break;
   }
   ```
   The trailing `g_lbl_cur ^= 1; g_refresh = 1;` is shared by all rows and must run.
6. **Write `my_apply()`** following the existing ones: set `g_config`, call whatever runtime
   function applies it live (if any), and `config_save_key("Key", "Value")`. If the setting
   can only apply at start-up, log that.
7. **Build and test.**

### Testing recipe

The Options list is opened from the pause menu. Scripted input uses the control file
(`MHGU_CONTROL=ctl.txt`, buttons uppercase, e.g. `PLUS 6`, `A 2`, `LEFT 2`), and `shot name`
writes `build/shots/name.png`. A workable test environment opens the pause menu straight into
a hunt and skips the intro:

```sh
cd build
MHGU_HIDDEN=1 MHGU_QUICKHUNT=101 MHGU_CONTROL=ctl.txt ./mhgurecomp
```

Wait for the hunt to load (~50 s), then drive the control file: `PLUS` opens the pause menu,
`A` opens Options, `A` on the Graphics row enters the submenu.

A useful test hook (not committed) parks the pager on the Graphics row so the navigation does
not have to scroll 27 rows:

```c
static int g_test_row = -1;
// in opt_update, before the refresh:
if (g_test_row >= 0 && g_opt_obj && !g_gfx_menu && LD32(g_opt_obj) == OPT_VPTR_RT) {
    ST16(PW_PAGE(g_opt_obj), (uint32_t)g_test_row / 8u);
    ST8 (PW_ROW(g_opt_obj),  (uint32_t)g_test_row % 8u);
    g_refresh = 1;
}
// in pausepages_init:
const char *t = getenv("MHGU_TEST_ROW"); if (t) g_test_row = atoi(t);
```

Then enter Options with `A`, then `A` enters the submenu, `DOWN` moves the row, `A` opens
the editor, `LEFT`/`RIGHT` change, `B` returns. Check `build/log.txt` for the
`[pausepages] ... -> ...` line and the INI for the written value.

Navigation is flaky in scripted runs (a press is sometimes eaten); retry the run rather than
assuming the code failed.

---

## 7. Constraints and gotchas

- **The borrowed block has five 7-byte value strings** (`+13/+20/+27/+34/+41`). Longer text
  overlaps the next slot; rows needing it must use the current-text/two-slot method.
- **Max value is 4.** Raising `out+0x0c` makes the window stop using the string list (it
  shows numbers or a neighbouring item's text). Do not exceed five values per row.
- **The value text comes from the borrowed block, not from the label buffer.** Writing value
  strings into the label buffer corrupts the label (it overwrites the label's NUL).
- **The borrowed block is saved and restored as 64 bytes.** If a new row writes beyond
  `+63`, enlarge `g_tbl_w` and `restore_tbl`.
- **Rows must stay last in the Options list.** Index-based stock code means inserting earlier
  breaks every option below.
- **A on index 27 is "Reset All Settings".** Keep it masked.
- **Left/right must be masked in the submenu.** Otherwise they edit Music Volume (the stock
  item under row 0) in addition to our row.
- **The help line is still the stock one** ("Set the volume of the background music."). It is
  produced by the window from the row under its cursor; fixing it means overriding the help
  text virtual/node, which has not been done.
- **The label pointer must change to redraw.** Flipping `g_lbl_cur` and re-running the layout
  is mandatory after every change.
- **`+0x60` is the total item count**, and the pager fields must be restored on exit.
- **FSR/FSR Quality and Resolution are start-up settings** (device features, upscaler, render
  scale, window/render-target size); FXAA, MSAA, sharpness, framerate and VSync apply live.
- **The nested Display view keeps the tall window** (four rows plus empty slots); the window
  height is not pager-driven. Shrinking it would mean resizing the window node, which is not
  done.
- **Framerate lives in the engine's limiter field** (`+0x3c`); the timing thread re-reads it
  each tick. Do not set it below 30 — the game's frame delta assumes 30 as the baseline.
- **VSync needs a swapchain rebuild**, so it is applied on the next frame, not in the input
  handler, to keep it on the render thread.

---

## 8. Quick reference

Files:

- `runtime/hle/pausepages.c` — the entry, submenu, row overrides, input.
- `runtime/hle/input.c` / `input.h` — press timestamps, calls `pausepages_input`.
- `runtime/config.c` / `config.h` — INI parsing and `config_save_key`.
- `runtime/nvn/vulkan/exec.c` — `fxaa_skipped()`.
- `runtime/nvn/vulkan/device.c` — `vk_set_msaa`, `vk_set_vsync`, FSR device features.
- `runtime/hle/vi.c` — frame rate target, `vi_set_framerate`, timing thread.
- `runtime/nvn/vulkan/fsr.c` — `g_fsr_on`, per-frame FSR dispatch and sharpness.
- `generated/main/main_102.c` — the Options window virtuals and layout.

Addresses:

| Name | Address |
| --- | --- |
| Options window object vptr | `0x1972c88` |
| item count virtual | `0x7e7534` |
| row virtual | `0x7e7558` |
| value virtual | `0x7e7d34` |
| window update | `0x7e5840` |
| row layout | `0x7e635c` |
| parameter setter | `0x85e5bc` |
| pause bar init / vptr | `0x877d0c` / `0x197aeb8` |

Pager (`obj + 0x4c` param): row `+0x59`, rows/page `+0x5a`, page `+0x5c`, page count `+0x5e`,
total `+0x60`; edit state `+0x74`.

Borrowed slot: item 4 ("Camera Angle"), block start `g_tbl`, value slots at
`+13, +20, +27, +34, +41` (7 bytes each), 64 bytes saved/restored.

INI keys: `Resolution`, `AspectRatio`, `FPS`, `VSync`, `FXAA`, `MSAA`, `FSR`, `Upscaler`,
`FSRQuality`, `Sharpness`.

Enabled: always (no switch).
