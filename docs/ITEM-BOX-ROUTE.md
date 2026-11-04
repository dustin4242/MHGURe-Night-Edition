# Reaching the item box with scripted input

How to take a fresh `mhgurecomp` from launch to the village item box (and into
`Manage Equipment → Select Equip.`) using nothing but `MHGU_CONTROL`. Useful for testing anything that needs the
equipment box — the armour-class feature, transmog, the box UI itself — without holding a controller.

The control-file format and the other commands are described in [STATUS.md](STATUS.md) (the `MHGU_CONTROL` entry): append one line per
action to the file the game was started with, `BUTTON [frames] [delay]`, plus `shot <name>` (writes
`shots/<name>.png`). Lines can be appended while the game runs, so a shell script is the usual driver.

## The whole route

```sh
cd build
rm -f ctl.txt
MHGU_CONTROL=ctl.txt SDL_VIDEODRIVER=x11 ./mhgurecomp >/dev/null 2>&1 &

# 1. boot: skip the logos, the intro movie, Continue and the character select
sleep 8
i=0; while [ $i -lt 10 ]; do echo "A 6 0" >> ctl.txt; sleep 3; i=$((i+1)); done
sleep 15                       # the village finishes loading

# 2. travel map: X, then A twice (Bherna is already selected)
echo "X 6 0" >> ctl.txt; sleep 3
echo "A 6 0" >> ctl.txt; sleep 3
echo "A 6 0" >> ctl.txt; sleep 3

# 3. walk forward into the house: four seconds of full tilt
echo "STICK_UP 240 0" >> ctl.txt; sleep 6

# 4. open the box
echo "A 6 0" >> ctl.txt; sleep 4
```

After step 4 the `Item Box` menu is open (`Store Items / Take Items / Item Sets / Combine/Manage Items /
Change Appearance / Manage Equipment / Equipment Sets / Change Style/Arts`).

| Step | Input | Delay after | What it does |
|------|-------|-------------|--------------|
| 1 | `A` ×10 | 3 s apart, first at 8 s | Skips the company logos, the intro movie, `Continue`, and the character select's `Load this character?` |
| — | (wait) | 15 s | The village has loaded once the HUD and the Moofah are drawn |
| 2 | `X` | 3 s | Opens the village travel map ("You can select any of the villages or the Hunters Hub to travel to") — [screenshot](images/item-box-travel-map.jpg) |
| 2 | `A`, `A` | 3 s each | Confirms Bherna and leaves the map after the warp |
| 3 | `STICK_UP 240` | 6 s | Walks forward for 4 s (240 frames at 60 fps) to the chest in the house; the prompt `Item Box` appears top-left — [screenshot](images/item-box-arrive.jpg) |
| 4 | `A` | 4 s | Opens the item box menu |

The first `A` presses are harmless: one that lands on the character-select dialog confirms it, and any that land in
the village do nothing (there is no NPC at the arrival point).

## Opening the equipment screen

```sh
# Item Box menu -> Manage Equipment (6th row)
i=0; while [ $i -lt 5 ]; do echo "DOWN 6 0" >> ctl.txt; sleep 2; i=$((i+1)); done
echo "A 6 0" >> ctl.txt; sleep 4       # -> Select Equip. / View Talismans / Set Decorations / ...
echo "A 6 0" >> ctl.txt; sleep 5       # -> the equipment box grid
```

The grid is the box (cursor starts on the first cell); the left side shows the piece under the cursor in the lower
panel and the piece currently equipped in that slot in the upper panel.

### Swapping a weapon (the armour-class case)

With the cursor on a weapon in the grid:

```sh
echo "A 6 0" >> ctl.txt; sleep 3       # pick it
echo "A 6 0" >> ctl.txt; sleep 6       # "Change equipment? (Changing weapons may result in armor being removed.)" -> Yes
```

Then `B` four times to back out of the box, and `PLUS` → page 2 → `Equipment` to check the result in the pause menu.

## Notes for testing equipment features

- **Back up the save first.** Equipment changes are written when the game saves or exits.
  Linux: `~/.local/share/MHGURecomp/save` (`MHGU_SAVE_DIR` moves it); Windows: `%APPDATA%\MHGURecomp\save`.
  ```sh
  cp -r ~/.local/share/MHGURecomp/save ~/.local/share/MHGURecomp/save.bak
  ```
- **While the box is open the game keeps every armour slot in `sPlayer` empty** (the equipped set lives in the
  box's own working copy and is written back when the box closes). Anything that reads or maintains those slots —
  `armor_class_frame()` in [research/armor-class.md](research/armor-class.md) — must cope with that: an entry
  snapshot taken before the box was opened is the set the player is wearing, even minutes later.
- **`MHGU_ARMOR_DEBUG=1`** logs one `[armor]` line per distinct lookup caller, the weapon class, and every
  `restore slot N` / `slot N empty` decision. Start the game with it when a report is about the armour-class feature.
- `shot <name>` writes the next presented frame to `shots/<name>.png`; `SHIFT+F12` in-game does the same from the
  keyboard.

## If it goes wrong

- **Still in the character select / main menu** — the first `A` presses were too early, or the game took longer to
  load. Add more `A` presses (3 s apart) before step 2; extra ones are harmless.
- **`X` did nothing** — it was pressed during the village's loading fade. Wait for the HUD, then press it.
- **Walking 4 s does not reach the chest** — the travel-map `A` pair did not warp (the map was already closing).
  Re-run step 2 and watch for the fade; the arrival point is fixed, so the 240-frame walk is always the same.
