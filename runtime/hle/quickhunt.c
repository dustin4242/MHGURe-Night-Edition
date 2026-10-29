// (debug) MHGU_QUICKHUNT=<quest number>: from start-up straight into that quest, for testing in a hunt (e.g. 101,
// Find the Ferns: Village 1 star, Jurassic Frontier, no large monster). Presses A through the title, the main menu
// (Continue), the character select (the first character) and the welcome message, then orders the quest and departs
// at the first update of the town the save is in. The save is not changed (no quest fee is paid).
//
// How the game departs: the counter's accept (0x984e94) writes the quest number to sQuestSelector +0xd68 (0 = none)
// and sets bit 0 of its flags (+0xd65); "Depart on Quest" at the departure point sets bit 3 there. The selector's
// update (vtable +0x1c, 0x5b7b94) then turns bit 3 into a request (+0x1e0 = 4) that lasts one update. In a town
// (aVillage*, aLobbyBase) the area's update switches on a mode (+0xb8); mode 1 (0x8bd0fc, the town as you walk
// around) runs the selector's update and, seeing the request and an ordered quest, calls the area's depart method
// (0x8c02a4): stage from the ordered quest (sGameControl +0x48 / +0x4a), the area's quest flag (+0xda), mode 4,
// whose states fade out and jump to the stage's area (aMap<nn>), where aQuest loads the quest from +0xd68.
//
// Timing (MHGU_LOG_FILES=1 + screenshots, 60 fps, quest 101): the save is first read at ~2.8 s (before the title),
// read again at ~8.2 s with the 250 ms cadence when Continue is chosen (at ~11.5 s with the old 1.5 s one); the
// front-end's NOW LOADING (town) runs ~13.5-17.9 s; the town is up at ~18 s, the quest arc is read at ~22.3 s and
// the map at ~22.7 s (playable by ~23.5 s). The old 90-frames-at-60-fps (1.5 s) reached the town at ~24.4 s.
// 250 ms (15 frames at 60 fps) is the fastest that survives the health screen / title / movie: 8 frames restarted
// the intro, 12 frames gave the same time as 15, and spamming A during the character screen did not shorten its
// ~5 s save-data wait (the game thread is blocked on the storage thread there). The remaining ~13.5 s to the town's
// loading screen and 4.4 s of town loading are game load time, not input. MHGU_QUICKHUNT_MS sets the A interval
// (default 250 ms), MHGU_QUICKHUNT_FIRST_MS the first press (default 2000 ms).
#include <stdlib.h>

#include "../hle.h"

#define FN_TOWN_IDLE     0x8bd0fcu   // town area update, mode 1 (r0: the area)
#define G_SELECTOR       0x1a8ae38u  // sQuestSelector *
#define SEL_FLAGS        0xd65u      // u8: bit 0 ordered at a counter, bit 3 depart
#define SEL_ORDERED      0xd68u      // the ordered quest's number
#define FLAG_ORDERED     0x01u
#define FLAG_DEPART      0x08u
#define MS(x)            ((uint64_t)(x) * 1000000ull)

static uint32_t g_quest;          // the quest to start (0: off)
static uint32_t g_ms = 250;       // milliseconds between A presses (MHGU_QUICKHUNT_MS)
static uint32_t g_first_ms = 2000; // milliseconds before the first press (MHGU_QUICKHUNT_FIRST_MS)
static uint64_t g_t0;             // when the presses started
static volatile bool g_in_town;   // set by the game thread at the first town update
static bool g_done;

static void town_idle(Cpu *c) {
    if (g_quest && !g_done) {
        g_done = true;
        g_in_town = true;
        uint32_t sel = LD32(G_SELECTOR);
        if (sel) {
            ST32(sel + SEL_ORDERED, g_quest);
            ST8(sel + SEL_FLAGS, LD8(sel + SEL_FLAGS) | FLAG_ORDERED | FLAG_DEPART);
            LOG("[quickhunt] in town: ordered quest %u, departing\n", g_quest);
        } else {
            LOG("[quickhunt] in town, but there is no sQuestSelector\n");
        }
    }
    fnhook_call_original(c, FN_TOWN_IDLE);
}

// UI thread: A every MHGU_QUICKHUNT_MS until the game reaches a town. Time based, so the same setting works at any
// frame rate (the frame count it replaces was not: 90 frames is 1.5 s at 60 fps but 0.75 s at 120).
void quickhunt_poll(void) {
    static uint64_t next;
    if (!g_quest || g_in_town) return;
    uint64_t now = plat_now_ns();
    if (!next) next = g_t0 + MS(g_first_ms);
    if (now < next) return;
    next = now + MS(g_ms ? g_ms : 1);
    host_input_press("A", 8);
}

void quickhunt_init(void) {
    const char *e = getenv("MHGU_QUICKHUNT");
    if (!e || !*e || !(g_quest = (uint32_t)strtoul(e, NULL, 10))) return;
    const char *ms = getenv("MHGU_QUICKHUNT_MS");
    if (ms && *ms) g_ms = (uint32_t)strtoul(ms, NULL, 10);
    const char *first = getenv("MHGU_QUICKHUNT_FIRST_MS");
    if (first && *first) g_first_ms = (uint32_t)strtoul(first, NULL, 10);
    g_t0 = plat_now_ns();
    fnhook_set(FN_TOWN_IDLE, hle_register("quickhunt:town_idle", town_idle));
    LOG("[quickhunt] quest %u: pressing A to the town every %u ms (first after %u ms), then departing\n",
        g_quest, g_ms, g_first_ms);
}
