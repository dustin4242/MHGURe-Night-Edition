# Quickhunt: where the time goes, and how much of it can be removed

`MHGU_QUICKHUNT=<quest>` (runtime/hle/quickhunt.c) drives the front end with A presses, orders the quest at the
first town update and departs. This note records what tracing found on the way and the faster default that came out
of it. Numbers are quest 101, Linux, the build at the time (60 fps unless said otherwise), `MHGU_HIDDEN=1`.

## Instrumentation

No new tracing hook was needed; the existing ones answer the question:

| What | How |
| --- | --- |
| save file I/O, with offsets and times | `MHGU_LOG_FILES=1` and `MHGU_SAVE_DIR=<copy>` (runtime/services/fs.c) |
| which guest functions a phase runs first | `MHGU_CONTROL` `fnnew <lo> <hi> <learn ms>`, re-armed between phases |
| one function/object's calls | `fntrace <lo> <hi> <n> [r0 lo r0 hi]` |
| screen state and exact playable moment | `shot <name>` at fixed times, then read `shots/*.png` |
| presented frame marker | `frame` (writes `[control] frame N`) |

A small host-side harness that starts the game with a pty, timestamps each log line and appends the control lines at
wall-clock times makes the timeline readable. The game writes `log.txt` when stderr is not a terminal, so the pty
matters.

Example (adjust the paths):

    cp -r ~/.local/share/MHGURecomp/save/save_0 /tmp/quickhunt-save/save_0
    cd build
    MHGU_HIDDEN=1 MHGU_SAVE_DIR=/tmp/quickhunt-save MHGU_QUICKHUNT=101 \
    MHGU_LOG_FILES=1 MHGU_CONTROL=/tmp/ctl.txt ./mhgurecomp 2>log.txt &
    printf '10.5 fnnew 200000 2347000 1\n' >> /tmp/ctl.txt

`fnnew` writes `addr from caller r0` for every function in the range it sees for the first time. Two runs are
usually enough: one learn window over the front end, one re-armed just before the town. `fntrace` is better for a
specific object (`r0` filter) or a function cluster; a full-range trace is per-frame noise after a few thousand
lines.

## Timeline (quest 101)

| Time (s) | Event |
| --- | --- |
| 0.4 | runtime up, quickhunt armed |
| 2.8 | save read #1: `save_0/system` +0x10, +0x10 0x80000, then +0x80010 0x46b8ac (system, system_backup, system, system: four mounts) |
| 3.5 | title arcs, `title.arc` / `title_eng.arc` |
| 7.5-8.6 | opening movie (`movie06.mp4`); the fast A presses skip it, its decoder stops by ~8.6 |
| ~8.2 | save read #2 when Continue is chosen |
| ~8.3 | character select loads (`player/lobby/make.arc`, the hunter's weapon/armour arcs), welcome message |
| 13.5 | `NOW LOADING` (the town) |
| 17.9 | first town update; quickhunt writes `sQuestSelector +0xd68` and flags `+0xd65` (ordered + depart) |
| 20.3 | quest card / fade-out starts (`area` depart 0x8c02a4, mode 4) |
| 22.3 | `loc/arc/quest/q0000101.arc` |
| 22.7 | `arc/stage/m01d/a00.arc` (Jurassic Frontier) |
| 23.5 | `sound/bgm/map/m01d/wav/m01a00d.opus`; playable (time-limit HUD on the next screenshot) |

With the old 90-frames-at-60-fps (1.5 s) cadence the second save read was at ~11.5 s and the town at ~24.4 s, i.e.
the whole run was ~30 s. 250 ms moves the second save read to ~8.2 s and the town to ~18 s. At 120 fps
(`MHGU_FRAMERATE=120 MHGU_VSYNC=0`) the same 250 ms reached the town at ~17.0 s and the map at ~20.8 s, because
the game's logic and loads run faster with the clock.

## What is input-bound and what is not

- **Input-bound:** the front-end menus. The title / Continue / character select path is reached at ~8.2 s instead
  of ~11.5 s just by pressing A more often.
- **Not input-bound:** the ~5 s after Continue. `fnnew` shows no new function in the character-screen range for five
  seconds, and `MHGU_FNPROF=1` stops printing its 5 s report, so the game thread is waiting on the save system's
  storage thread (thread 0x11f) while the character screen is up. Spamming A every 100 ms from 8.4 s to 13.2 s moved
  the town by 0.16 s (17.83 vs 17.99). The welcome message / model load ignores early input; do not chase it with
  more presses.
- **Not input-bound:** the 4.4 s town `NOW LOADING` (13.5-17.9 s) and the ~5 s from the depart to a playable stage
  (18-23.5 s: fade, quest card, quest arc + map arcs, BGM).

Cadence experiments: 250 ms (15 frames at 60 fps) is the fastest that survives the health screen, title and movie.
8 frames (133 ms) restarted the intro movie and never reached the town; 12 frames (200 ms) gave the same town time
as 15. The first press can be at 90 frames (1.5 s) or 120 frames (2 s) with no difference; 2 s is the default.

## The pins this produced (MAIN v1.4.0, runtime addresses)

| Address | What |
| --- | --- |
| `0x8bd0fc` | town area update, mode 1; quickhunt's hook point (`fnhook_set`) |
| `0x8c02a4` | the town's depart method (called from 0x8bd228 inside 0x8bd0fc) |
| `0x1a8ae38` | `sQuestSelector *`; `+0xd65` flags (bit 0 ordered, bit 3 depart), `+0xd68` ordered quest |
| `0x5b7b94` | selector update (vtable +0x1c), turns bit 3 into request `+0x1e0 = 4` for one update |
| `0x984e94` | quest counter accept: writes `+0xd68`, sets flag bit 0 |
| `f_a794cc` | `sSavedata` class registration (size 0xfa0), main_141.c |
| `f_a771f0` | `sSavedata` ctor; the storage thread and its event are set up here (`sSavedata::cStorageThread::mEvent_%08x` is at 0xa77264) |
| `f_827af8` | registers `cUIOSaveData` / `cUIOSaveData::cUIOSaveSlot` (main_105.c) |
| `f_896230` | registers `uUITitleMenu` (main_112.c); `f_20b960` registers `aTitle` (main_000.c) |
| `f_205ab0` | registers `aMapBase`; `f_201fe8` `aMap01`; `f_21095c` `aVillageBase` (main_000.c) |
| `f_212a5c` | registers `sFestaArea` (the area singleton, size 0x3848) |

The save load itself is not on the game thread: thread 0x11f runs `IFileSystem` reads via the SDK, and its stack at
the read contains only SDK frames. `MHGU_LOG_FILES` is therefore the useful view of the save path at runtime; the
guest-side pins above come from the class registrations / `retool.py dti`.

## Where the next 8-10 s are

The town is only an intermediate: the save's location forces `aVillage*` to be created after Continue, and only
that area's mode 4 knows how to depart to `aMap<nn>`. To remove the 13.5-17.9 s town load, the area creation has to
be redirected before it loads town assets:

1. Hook the area change (the `sFestaArea` singleton, `f_212a5c` registers it) or `aVillageBase`'s constructor.
2. Write the quest to `sQuestSelector` (`+0xd68`), set the same bits quickhunt sets, and set `sGameControl`'s stage
   (`+0x48`/`+0x4a`) and the area's quest flag (`+0xda`)/mode (`+0xb8`) before the first update.
3. The target area then has to be `aMap<nn>` directly. That is the risky part: `aQuest` expects the depart
   method's setup, not just a stage id. The Arena case ("no function at 0" when departing) is a known failure mode
   of calling the depart path without that context.

A safer intermediate, if it is worth the work: hook the character-screen exit and issue the quest order before the
town's first update, so the ~0.2 s in `town_idle` disappears; it is noise compared with the town load. The current
fast path (250 ms, ~23.5 s at 60 fps, ~21 s at 120) is the practical floor without touching area creation.

## Result

The default A cadence is now time based, so it is the same on every frame rate:

| Variable | Default | Meaning |
| --- | --- | --- |
| `MHGU_QUICKHUNT` | off | quest number |
| `MHGU_QUICKHUNT_MS` | 250 | ms between A presses |
| `MHGU_QUICKHUNT_FIRST_MS` | 2000 | ms before the first A press |

With defaults, quest 101 is playable at ~23.5 s at 60 fps (was ~30 s) and ~21 s at 120 fps.
