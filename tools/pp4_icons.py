#!/usr/bin/env python3
"""Extract the controller-glyph textures from the pp4 Layout variants.

The pause/equipment menus and the HUD keep their button glyphs in five TEX textures. Each variant
(Switch / Xbox / Playstation, HD icons) has the same textures with only the glyph pixels changed.
This writes the texel payloads (the TEX file after its 0x18-byte header) to

    <out>/Switch/<id>.bin, <out>/Xbox/<id>.bin, <out>/Playstation/<id>.bin
    <out>/icons.txt        icon id size fnv1a64

so the runtime can match a live NVN texture by (size, hash) and copy another set over it.

Usage: pp4_icons.py <addons/pp4/English/Layout> <out-dir>
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mtarc import Arc  # noqa: E402

VARIANTS = (("Switch", "2-NS"), ("Xbox", "3-XB"), ("Playstation", "1-PS"))
# id -> (archive relative to the variant folder, entry name)
TEXTURES = {
    "HD_custom_pnl2":     ("resident_eng.arc", "eng\\GUI\\99_texture\\HD_custom_pnl2_BM_NOMIP"),
    "HD_window":          ("resident_eng.arc", "eng\\GUI\\99_texture\\HD_window_BM_MQ_NOMIP"),
    "HD_lobby00":         ("resident_eng.arc", "eng\\GUI\\99_texture\\HD_lobby00_BM_NOMIP"),
    "HD_hud00":           ("quest_eng.arc",    "eng\\GUI\\99_texture\\HD_hud00_BM_NOMIP"),
    "button_festa":       ("quest_eng.arc",    "eng\\GUI\\02_common\\font\\button_festa_00_ID"),
    # The shared loc arc holds the fonts the menus actually draw the button prompts from.
    "HD_custom_pnl2_loc": ("loc/arc/resident.arc", "GUI\\99_texture\\HD_custom_pnl2_BM_NOMIP"),
    "HD_window_loc":      ("loc/arc/resident.arc", "GUI\\99_texture\\HD_window_BM_MQ_NOMIP"),
    "button_festa_loc":   ("loc/arc/resident.arc", "GUI\\02_common\\font\\button_festa_00_ID"),
}


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def read_tex(arc_path, entry_name):
    arc = Arc.open(arc_path)
    for e in arc.entries:
        if e["name"] == entry_name:
            data = arc.read(e)
            if data[:4] != b"TEX\0":
                raise SystemExit(f"{arc_path}: {entry_name}: not a TEX file")
            size = struct.unpack_from("<I", data, 0x10)[0]
            if 0x18 + size > len(data):
                raise SystemExit(f"{arc_path}: {entry_name}: short TEX payload")
            return data[0x18:0x18 + size]
    raise SystemExit(f"{arc_path}: entry not found: {entry_name}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    layout, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    manifest = []
    cache = {}
    for icon, var in VARIANTS:
        os.makedirs(os.path.join(out, icon), exist_ok=True)
        for tex_id, (arc_name, entry) in TEXTURES.items():
            key = (var, arc_name)
            if key not in cache:
                under = arc_name if arc_name.startswith("loc/") else os.path.join("HDI", arc_name)
                cache[key] = os.path.join(layout, var, under)
            payload = read_tex(cache[key], entry)
            with open(os.path.join(out, icon, tex_id + ".bin"), "wb") as f:
                f.write(payload)
            manifest.append(f"{icon} {tex_id} {len(payload)} {fnv1a64(payload):016x}")
            print(f"icons: {icon:11s} {tex_id:14s} {len(payload):8d} {fnv1a64(payload):016x}")
    with open(os.path.join(out, "icons.txt"), "w") as f:
        f.write("\n".join(manifest) + "\n")


if __name__ == "__main__":
    main()
