// Hot-swappable controller glyphs (Switch / Xbox / Playstation). The pp4 Layout variants keep the button
// glyphs in five TEX textures; the build extracts their texel payloads into data/pp4_icons and this module
// swaps the live NVN textures when [Controls] Icons changes (runtime/ui_icons.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct NvnTexture;

bool icons_available(void);                  // data/pp4_icons/icons.txt exists (the pp4 glyph sets were built)
void icons_init(void);                       // read data/pp4_icons/icons.txt and preload the selected set
const char *icons_name(int v);               // ICONS_* -> "Switch" / "Xbox" / "Playstation"
void icons_set(int v);                       // change the setting live (and write it to the INI)
// Called by the renderer before uploading a texture: tag it by its (size, hash) and, if it is one of the
// glyph textures and the selected set differs, replace its guest texels and update *hash.
bool icons_texture(struct NvnTexture *t, const uint8_t *mem, uint64_t total, uint64_t *hash);
