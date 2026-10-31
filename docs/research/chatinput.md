# In-game chat / text entry (how it works, and reusing it)

This note traces the chat text entry you get with **ZR + MINUS** and Enter, and what it would take to
reuse that input for something else (the plan is a typed resolution height in the Graphics menu).
Code: `runtime/hle/swkbd.c`, `runtime/textinput.c`, `runtime/host_input.c`; game side in
`generated/main/main_139.c`.

## Opening it

- **ZR (hold), then MINUS**, while playing in the village/hub (tested offline). The chat panel and
  its text box appear.
- It does **not** open while the pause menu (or the Options/Graphics windows) is up: the shortcut is
  handled by gameplay input, and a test with the pause menu open produced no keyboard call at all.
- In an offline solo hunt the shortcut produced nothing in our test; the village/hub works. Online
  hunts were not tested (the chat is the multiplayer feature).

The village pause bar's **Chat** tab opens `cUIOKaiwaExpWindow` (Chat Menu / Shoutout Reset); its
"Keyboard" action opens the same text entry while the pause menu is up. The chat panel itself is
`cUIOChatWindow` (`0x196db9c`), which is what draws the box.

## The text entry path

```
ZR+MINUS (gameplay input)
  -> chat system opens cUIOChatWindow / its entry box
  -> "Keyboard" action
  -> game wrapper f_a62d88 (main 0xa62d88)          builds an AppearArg and drives the keyboard
  -> nn::swkbd::InlineKeyboard methods              (HLE: runtime/hle/swkbd.c)
  -> runtime/textinput.c                            SDL text input: characters, Backspace, Enter, Esc
  -> ChangedString / DecidedEnter / DecidedCancel callbacks into the game
  -> the game's changed callback updates the visible text box
  -> the decided-enter callback sends the chat message
```

### HLE side (`runtime/hle/swkbd.c`)

The game only uses `nn::swkbd::InlineKeyboard` (not the full-screen `ShowKeyboard` applet). The HLE:

- `Appear(AppearArg)`: marks the keyboard shown and calls `textinput_begin(initial, max)`.
  `AppearArg` layout: `u32 mode`, `char16 ok_text[9]`, 2 × `char16` optional symbol keys, 2 × bool,
  `u32` invalid characters, **`s32 max length` at `+0x20`**, `s32 min length` at `+0x24`.
  For the chat entry the max is **26** (`0x1a` in the log dump).
- `Calc()`: the state machine the game polls: 0 not initialized, 1 ready/hidden, 3 shown. On a host
  change it calls the game's ChangedString and MovedCursor callbacks; Enter calls DecidedEnter with
  the text; Escape calls DecidedCancel.
- Callback argument (`ChangedStringArgUtf8` / `DecidedEnterArgUtf8`): UTF-8 text at `+0`, character
  count at `+0x7d4`, cursor (and dictionary range) at `+0x7d8`.
- `SetInputText`: the game supplies the starting text (the name being edited).
- `Disappear` / `Finalize`: `textinput_end()`.

### Host side (`runtime/textinput.c`)

While a text entry is active, SDL text input is enabled and keys stop acting as controller buttons
(`host_input.c` asks `textinput_event` first). Characters go into the buffer, Backspace deletes,
Ctrl+V pastes, Enter = `TI_ENTER`, Escape = `TI_CANCEL`. The HLE polls it from `Calc` on the game's
thread (`textinput_poll`).

Test controls: `MHGU_CONTROL` has `type <text>` and `textkey enter|escape|backspace`; `MHGU_NAME`
answers any keyboard automatically, `MHGU_INLINE_CANCEL=1` cancels instead.

### Game side (`main_139.c`)

- The caller of `InlineKeyboard::Appear` is `f_a62d88` at `0xa62ed4`, the game's keyboard wrapper
  (it fills the AppearArg from its object: initial text, max length from object `+0x3b10`, and so on).
- The chat's **decided-enter callback is `f_a63b54`** (registered at boot). It copies the entered
  text to the chat object at `+0x2ed8` (0x7d1 bytes), stores the character count at `+0x36b0`, sets a
  flag at `+0x36ac`, and calls the chat's send path.
- The **changed** callback writes the text into the visible box; the box itself is drawn by the game
  (`cUIOChatWindow`), not by the HLE. The HLE only supplies the text.

## The chat driver, and forcing it from the pause menu

The chat input is a manager object (heap, e.g. `0x15c60060`) that the HUD update `f_a6b5a0` finds
at `LD32(hud + 0x154)` and steps through `f_a62570` (`0xa62570`) every gameplay frame. Its state is
at `+0x2ec8`; a real ZR+MINUS entry goes **1 → 2 → 3 → 4** (4 opens the keyboard), and after Enter
it goes **8 → 9 → 3**. The keyboard wrapper `f_a62d88` is called from state 4.

So the keyboard can be opened from our menu without gameplay input: capture the manager from the
HUD update hook, set `+0x2ec8 = 4`, and call `f_a62570(manager)` from the Options update each frame.
Tested in-hunt with the Graphics menu open — the HLE keyboard opened (`[swkbd] inline keyboard
shown`) and typing/Enter worked (`[swkbd] entered "42"`).

**But the visible box did not appear.** The box is drawn by the chat window
(`cUIOChatWindow`, global object `0x0c07dcb0`, vptr `0x196db9c`), which is hidden while the pause
menu is up (visibility byte `+0x18` = 0, and the HUD is not drawn during the pause). Setting
`+0x18 = 1` from the pause made the pause input path call the window (`f_77767c`) and it crashed
with `FATAL: no function at 0` — the window's pause-mode callbacks are only set up when the
pause bar's Chat tab opens it.

So: the *input* can be driven from the menu, but the *chat overlay visual* cannot simply be
composited over the pause menu. Drawing it would mean running the chat window as a pause window
(the way the village Chat tab does) or drawing a field ourselves.

## Why this matters for the Resolution height field

The overlay people see is the **game's chat window**. The HLE keyboard does not draw a box, so
"open the keyboard" alone gives no visible field. To show that overlay, the chat window has to be
open, and it is only opened by gameplay input (ZR+MINUS) — not from the paused Options menu.

Consequences for "type the height in that overlay":

1. **Open the chat from the menu**: not possible directly (tested: ZR+MINUS while paused does
   nothing).
2. **Close the menu, send ZR+MINUS, capture the text, reopen the menu**: works in the village/hub,
   but the solo hunt did not open the chat, and the entered text would have to be intercepted at
   `f_a63b54` (or the HLE decided-enter) so no chat message is sent. Clunky and inconsistent between
   village/hunt/online.
3. **Menu-local numeric entry (no chat window)**: on the Resolution row, call
   `textinput_begin(NULL, 4)` and poll `textinput_poll` from the window update. Show the typed digits
   in the row's value string (the same two-slot/relayout machinery the other rows use), Backspace
   edits, Enter applies the height, Escape cancels. This reuses the *input plumbing* (SDL text input)
   but not the chat box, and it works wherever the Options menu works.
4. **Drive the chat manager + show the digits in the row**: the forced-state-4 path above already
   makes the game's inline keyboard the input; its changed-string callback could feed the Resolution
   row's value instead of the (invisible) chat box. Same visuals as option 3, but through the game's
   chat input manager.

**Implemented: option 3.** The Resolution height row starts `textinput_begin(NULL, 4)` with
`textinput_digits_only(true)` (a filter added to `runtime/textinput.c` that drops anything outside
0-9), shows the digits and a blinking `|` in the row, applies on Enter (240..4320) and cancels on
Escape/B. The chat manager/window is not involved, so there is no chat message and no crash.

Option 3 is the practical one. Option 2 is the only way to get the actual chat-style box, and it
would need the gameplay chat to be available in the current mode.

## Reference

| What | Where |
| --- | --- |
| InlineKeyboard HLE | `runtime/hle/swkbd.c` |
| Host typing | `runtime/textinput.c` |
| SDL event routing | `runtime/host_input.c` (`textinput_event`, `type`/`textkey`) |
| Chat window class | `cUIOChatWindow` `0x176db94` → `0x196db9c`, `generated/main/main_096.c` |
| Village chat tab | `cUIOKaiwaExpWindow` `0x1777bb8` → `0x1977bc0`, `generated/main/main_106.c` |
| Keyboard wrapper | `f_a62d88` (`0xa62d88`), calls `Appear` at `0xa62ed4` |
| Chat decided-enter | `f_a63b54` (`0xa63b54`) |
| AppearArg | max length `+0x20`, min `+0x24` |
| Callback args | text `+0`, chars `+0x7d4`, cursor `+0x7d8` |
