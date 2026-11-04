// Two-button inputs (the Hunting Horn's X+A, the Lance's X+A+R...) at high frame rates. The player's input update
// (0x47ca28, every weapon) holds a press for a moment to see whether another button joins it: it sets bit 0 of the
// player's input bits (+0x1444) and a countdown (+0x1448 = 2.0, 0x47d558), takes 1.0 off the countdown each update
// (0x47ca84: `vmov.f32 s2, #-1.0`, then `vadd`) and gives the press on its own when it reaches 0. Two updates: 67 ms at
// the Switch's 30 updates a second, 17 ms at 120 fps, 8 ms at 240, so both buttons had to go down almost together.
// The patched constant takes s = step / 2 updates off instead (step: the engine's frame step in 60 fps frames, s = 1
// at 30 fps; at most 1, below 30 fps as the original): the window lasts 67 ms at any frame rate (tools/gen.py PATCHES,
// "Two-button input window"). [Controls] ComboInputWindow = Strict keeps 1.0 per update.
#include "../config.h"
#include "../hle.h"

#define G_SYSTEM 0x2326764u  // engine system object *: +0x68 the frame step in 60 fps frames

float combowin_tick(void) {
    if (config_live_combo_window() == COMBO_STRICT) return 1.0f;
    uint32_t sys = LD32(G_SYSTEM);
    float s = sys ? LDF(sys + 0x68) * 0.5f : 1.0f;
    return s > 0.0f && s < 1.0f ? s : 1.0f;
}
