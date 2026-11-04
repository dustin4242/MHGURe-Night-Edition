// The buff icons on the HUD's status plates (yours, the other hunters', the Palicoes': cUIOStatusPlate /
// cUIOStatusPlateGuests) take turns when more than one is active. The plate update calls 0x834d00 once per game
// update for each of its two icon slots; the slot's {index, countdown, shown} (plate +0xf0 / +0xfc) counts down by 1
// each update and moves to the next active buff at 0, reloaded with 20 (0x834ef4): 0.67 s at the Switch's 30
// updates a second, 0.08 s at 240 fps. The patched decrement (0x834d20) takes s = step / 2 updates off instead
// (step: the engine's frame step in 60 fps frames, s = 1 at 30 fps; at most 1, below 30 fps as the original), in
// 1/65536 units, and the reload is 20 of those (tools/gen.py PATCHES, "Buff icon cycle").
#include "../hle.h"

#define G_SYSTEM 0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames
#define UNIT 65536

uint32_t buffcycle_reload(void) { return 20u * UNIT; }

uint32_t buffcycle_tick(void) {
    uint32_t sys = LD32(G_SYSTEM);
    float s = sys ? LDF(sys + 0x68) * 0.5f : 1.0f;
    if (!(s > 0.0f && s < 1.0f)) s = 1.0f;
    uint32_t t = (uint32_t)(s * UNIT + 0.5f);
    return t ? t : 1;
}
