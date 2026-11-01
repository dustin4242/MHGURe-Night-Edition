// Hot-swappable HD UI (ui_hd.c). The pp4 HD UI arcs replace whole archives and many of their
// textures are 2x or 4x larger than the stock ones, so this cannot be a plain payload copy like
// the controller glyphs: a texture whose variant size differs is rebuilt at the target size
// (its guest storage points at a host buffer holding the stock payload, and the backend image is
// recreated on the next draw). Textures that are the same size are swapped through the same
// buffer path, which keeps the original storage intact for switching back.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct NvnTexture;

bool hd_ui_available(void);   // data/pp4_ui/ui.txt exists (the pp4 HD UI set was extracted)
void hd_ui_init(void);        // read the manifest
void hd_ui_identify(struct NvnTexture *t, uint64_t total, uint64_t hash);
                              // called from the renderer when a texture's content hash is known
void hd_ui_tick(void);        // periodic: apply the current On/Off state to live textures
void hd_ui_set(int on);       // change the setting live (and write it to the INI)
