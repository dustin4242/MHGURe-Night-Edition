// (debug) F2: the player's position and facing; the control file's "tp" moves him. The player object (sPlayer +0x1c:
// the four player slots, ours is sGameControl +0x30) keeps its world position at +0x40 (three f32, cm: dummy.c places
// monsters from it) and its rotation at +0x50 (a quaternion, the same one dummy.c turns the hunter's forward with),
// made each update from the facing angle at +0xfec (a u16, 0..65535 = 0..360 degrees; +0x14a0 its copy).
#include "../hle.h"
#include <math.h>

#define G_SPLAYER      0x1a85b58u  // sPlayer *: +0x1c the four player slots
#define G_GAMECONTROL  0x1a8ca48u  // sGameControl *: +0x30 our player slot
#define OBJ_POS        0x40u       // player: the world position (three f32)
#define OBJ_ROT        0x50u       // player: the rotation (quaternion x, y, z, w, made from the angle below)
#define OBJ_ANGLE      0x0fecu      // player: the facing, a u16 angle (0..65535 = 0..360 degrees)
#define OBJ_ANGLE2     0x14a0u      // its copy (the game moves both together; the quaternion at +0x50 is made from them
                                    // each update, so writing the quaternion alone does not last)

static uint32_t local_player(void) {  // (dummy.c's: the hunter's object, 0 outside a quest and in the menus)
    uint32_t gc = LD32(G_GAMECONTROL), sp = LD32(G_SPLAYER);
    uint32_t slot = gc ? LD32(gc + 0x30) : 4;
    return sp && slot < 4 ? LD32(sp + 0x1c + 4 * slot) : 0;
}

// F2 (the UI thread, as the control file's peeks are): the game writes these words in step, so the log reads them a
// frame late at worst.
void playerpos_log(void) {
    uint32_t pl = local_player();
    if (!pl) {
        LOG("[playerpos] no player yet\n");
        return;
    }
    float x = LDF(pl + OBJ_POS), y = LDF(pl + OBJ_POS + 4), z = LDF(pl + OBJ_POS + 8);
    float qx = LDF(pl + OBJ_ROT), qy = LDF(pl + OBJ_ROT + 4), qz = LDF(pl + OBJ_ROT + 8), qw = LDF(pl + OBJ_ROT + 12);
    float fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy);  // the hunter's forward, as dummy.c has it
    float yaw = atan2f(fx, fz) * 57.29578f;
    if (yaw < 0) yaw += 360.0f;
    LOG("[playerpos] %llu pl %08x pos %.2f %.2f %.2f rot %.4f %.4f %.4f %.4f yaw %.1f\n",
        (unsigned long long)g_present_count, pl, x, y, z, qx, qy, qz, qw, yaw);
}

// The control file's "tp x y z [yaw]" (tests): the position and facing the game itself keeps (the angle, not the
// quaternion, which is made from it each update; yaw < 0 keeps the current facing). Written from the UI thread like
// "poke"; the game takes it as its own on the next update, then grounds him (y follows the terrain).
void playerpos_teleport(float x, float y, float z, float yaw) {
    uint32_t pl = local_player();
    if (!pl) {
        LOG("[playerpos] tp: no player yet\n");
        return;
    }
    STF(pl + OBJ_POS, x), STF(pl + OBJ_POS + 4, y), STF(pl + OBJ_POS + 8, z);
    if (yaw >= 0) {
        uint16_t a = (uint16_t)lrintf(yaw * 182.04444f);  // degrees -> 65536 per turn
        ST16(pl + OBJ_ANGLE, a), ST16(pl + OBJ_ANGLE2, a);
        LOG("[playerpos] tp: pos %.2f %.2f %.2f yaw %.1f\n", x, y, z, yaw);
    } else {
        LOG("[playerpos] tp: pos %.2f %.2f %.2f\n", x, y, z);
    }
}
