# The title Main Menu (2026-10-30)

Goal (on hold): drop Download and Data Transfer from the Main Menu (both need Nintendo's servers and do nothing in
the port) and put a Quit item in their place: New Game, Continue, Options, Quit. Nothing is implemented yet; this is
what was found, main v1.4.0, runtime addresses (main at 0x200000).

## The title sequence's state machine

- One object drives the whole title sequence (logos, intro movie, "Press A", Main Menu, Character Select, Save Data
  Transfer...). Its vtable is at 0x197b094, right before uUITitleMenu's (0x197b0fc); its class name is not known yet.
  Slot 9 (vtable +0x24) is its update, 0x87b544. The object seen: 0xc100060 (heap).
- The state is the byte at object +4, switched by a 32-entry jump table at 0x87b600 (offsets from 0x87b600):
  0x00 0x87b680, 0x01 0x87b6a4, 0x02 0x87b70c, 0x03 0x87b744, 0x04 / 0x09 0x87b77c, 0x05 0x87b790, 0x06 0x87b838,
  0x07 0x87b84c, 0x08 0x87b890, 0x0a 0x87b8bc, 0x0b 0x87b904, 0x0c 0x87b930, 0x0d 0x87b954, 0x0e 0x87ba44,
  0x0f 0x87ba80, 0x10 0x87bb28, 0x11 0x87bd54, 0x12 0x87bd94, 0x13 0x87bf10, 0x14 0x87c054, 0x15 0x87c19c,
  0x16 0x87c25c, 0x17 0x87c5ac, 0x18 0x87c668, 0x19 0x87c6a0, 0x1a 0x87c72c, 0x1b 0x87c950, 0x1c 0x87c9d4,
  0x1d 0x87dd7c (nothing), 0x1e 0x87cb78, 0x1f 0x87cc28.
- Seen from start-up with the test save (states logged as they changed): 0x02, 0x03, 0x07, 0x0e (start-up and the
  intro movie), 0x0f (frame 810: probably the "Press A" title), 0x10, 0x11 (the Main Menu, frame ~1130). Picking Data
  Transfer went to 0x16 (the "Save Data Transfer" screen); backing out of it returned to 0x11.
- uUITitleMenu itself (vtable 0x197b0fc, update 0x896794, open 0x89656c) never ran on the Main Menu: it is some
  other title screen (Options?), not the Main Menu.

## The Main Menu's cursor

- A menu cursor object at title object +0x600 (class: ctor 0x74ed74, setup 0x74edec, update 0x74ee98):
  +0x8 the last move (2 down, 1 up, 0 none), +0xc 1, +0xd the current index (u8, wraps), +0xe the item count (u8),
  +0x10 / +0x12 page fields, +0x14 the count again (u16), +0x18 / +0x1c mode words. Title object +0x60d / +0x60e are
  the index / count; +0x610 (u16) is written from them (index / count).
- setup 0x74edec(cursor, count, r2, r3) stores the count; the Main Menu calls it at 0x87d70c with `mov r1, #5`
  (0x87d708). Another path (0x87d778) restores the index from [global] +0x6d modulo the count at +0x60e. So a count
  of 4 there would make the cursor wrap over the first four items.
- update 0x74ee98(cursor, pad bits) is called at 0x87d820 with the pad's buttons (the global at 0x87d808 +
  0x6bfc | +0x6c34) and returns nonzero when the cursor moved. Then:
  - index 4 (Data Transfer): help text 0x85e5bc(.., 5, 0xb, 0) - another message file;
  - else: help text 0x85e5bc(.., 5, 1, 0x17 + index).
  The decision itself (Confirm) and where each item goes is in state 0x11's handler (0x87bd54, then 0x87d444...),
  not traced yet.

## The help line under the menu

- 0x85e5bc(r0 2|4, gui 5 (the description box), message file, message index, [sp], [sp+4]) sets it through
  0x8429a4 (a GUI object's message).
- Message file 1 is TitleMsg (eng/table/TitleMsg_eng.gmd, also in title_eng.arc): 0x17 "Start a new game.",
  0x18 Continue's, 0x19 Options', 0x1a "View Downloadable Content." (Download). Data Transfer's (file 0xb, message 0)
  is presumably HD_DataTransfer_eng.gmd. A Quit item would want its own text in place of 0x1a.

## The bar labels are pictures

- "New Game", "Continue", "Options", "Download" and "Data Transfer" are not text anywhere in the game files (not in
  any gmd, gui or the exe, in UTF-8 or UTF-16, loose or in an arc). They are drawn from the texture
  `eng\GUI\99_texture\HD_title00_BM_NOMIP` (1024x1024, BC3), its top left: "DATA TRANSFER" (caps, y ~0-45), then
  New Game, Continue, Options, Download (x ~10-290, about 47 px a line, to y ~240), and "Data Transfer" again at
  y ~385-430. The same texture holds the screen titles (Main Menu, Character Select, Options, Download...: the
  blue-outlined font) and the "PRESS (A)" picture. Every language has its own HD_title00 (fre, ger, ita, spa, chS,
  chT) with its own words. The game has no "Quit" / "Exit" label picture.
- Confirmed with the renderer's dump on the Main Menu: its draws sample a 1024x1024 NVN format 0x44 texture (12
  draws, HD_title00), a 512x512 0x44 one (9 draws: HD_lobby00_BM_NOMIP, the bars' end ornaments), and a 1024x1024
  0x42 (BC1) one.
- The labels' lettering is the festa font, eng\GUI\02_common\font\font_festa.gfd ("GFD", 0x53e6 glyphs). A Quit
  label could be drawn into the texture where "Download" is, from that font's glyphs, when the texture is loaded
  (localized: Quitter, Salir, Beenden, Esci). The gfd's layout and its glyph textures are not decoded yet.
- Other title textures (title_eng.arc): HD_title04 (512x1024, BC3: the Options / creation sub-menu labels in a plain
  font, "TV Brightness", "Create New Character"...), HD_title01 (the logo), HD_title_down (the background pattern),
  HD_download / HD_dl0x (Download screen pictures).

## The GUI layouts (eng/GUI/03_title, also in title_eng.arc)

- menu.gui: the upper part (logo, "attention", style board; sequences GameMode, Chara, Data, Option...), font
  festa, textures HD_title00 / HD_title01 / HD_lobby00.
- menu_down.gui (245 KB): most likely the Main Menu itself (Null_StartMenu, startBtn, TexS_line, Tex_text00..02,
  Null_StoreMenu01/02; textures HD_title00, HD_title04, HD_lobby00, HD_title_down...). Not confirmed which objects
  are the five bars.
- save.gui: Character Select's save slots. download*.gui: the Download screen. title_flow.gui, loading*.gui.
- The title object gets its GUIs by index with 0x85cb3c(object, n) (0, 1, 2, 5...); the code sets bits on them at
  +0x4c (bit 0) and +0x50 (0x4, 0x20, 0x40000, clears 0x8): probably show / hide / input flags, not checked.

## What is left for the Quit item

1. Hide the Data Transfer bar: find how the Main Menu's GUI shows its bars (menu_down.gui objects) and how game code
   hides one object of a layout.
2. Cursor count 4 (setup at 0x87d708, or a hook on 0x74edec when it is called from there).
3. The label: "Quit" in the festa font drawn into HD_title00 where "Download" is, as the texture is read from its
   arc (decode font_festa.gfd and its glyph texture; write the letters' alpha back as BC3 blocks).
4. The help line for index 3: hook 0x85e5bc when it asks for TitleMsg 0x1a on the Main Menu.
5. Picking index 3: find where state 0x11 acts on Confirm, and quit there the way the settings menu's Exit does
   (ui_request_action(false); ConfirmExit applies).

## How it was found

- Class names (uUITitleMenu, cUIOTitleMenu...) are referenced PC-relatively (`ldr rX, [pc, #n]` then
  `add rX, pc, rX`), so there are no absolute pointers to them in the image; a scan for that pair (and movw/movt)
  finds the code (uUITitleMenu: 0x896334, its type registration; cUIO* names: 0x20bb48..0x20bc9c).
- Runtime: a temporary hook (fnhook_set on the game function, logging r0..r3, the caller and the result, and the
  bytes at r0 + an offset when they change) on 0x87b544 (+4: the state, +0x600: the cursor) and 0x74ee98, with the
  test save, driven through MHGU_CONTROL (A x3 after ~16 s reaches the Main Menu; DOWN, shot).
- Textures: MT .tex header: u32 at +8: width = bits 6..18, height = bits 19..31; u32 at +0xc: format = bits 8..15
  (0x13 BC1, 0x17 BC3); level 0 at +0x18, block-linear as runtime/nvn/tegra.c (GOB 64 bytes x 8 rows, block height
  from the height in blocks, at most 16 GOBs). White label art keeps its shape in alpha only: draw it over a dark
  background to see it. The Main Menu frame's textures came from MHGU_CONTROL "dump" with a log line per draw.
