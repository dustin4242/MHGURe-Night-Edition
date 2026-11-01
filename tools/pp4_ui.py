#!/usr/bin/env python3
"""Extract the pp4 HD UI textures and their stock counterparts.

The HD UI pack replaces whole arc files, and many of its textures are larger than the stock
ones (2x or 4x in each dimension).  This writes every differing TEX payload's *stock* bytes to

    <out>/<id>.bin
    <out>/ui.txt        id hd_size hd_hash sd_size sd_hash icon

so the runtime can match a live NVN texture by storage size + FNV-1a and either copy the stock
payload (same size) or rebuild the texture at the stock dimensions (scaled down by the size
ratio).  The HD payloads are not copied: the game already loads them from the merged arcs, and
the live storage is restored when HD UI is turned back on.

Sources (the same set the Makefile merges):
  English/HD UI v4/romfs                the 206 UI/map/tutorial/etc. arcs
  English/Layout/<variant>/HDI          resident_eng.arc / quest_eng.arc (pause, equipment, HUD)
  English/Layout/<variant>/loc/arc      the shared button-font resident.arc
  English/Layout/<variant>/title_eng.arc

Usage: pp4_ui.py <addons/pp4> <romfs> <out-dir> [<variant>, default 2-NS/HDI]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mtarc import Arc  # noqa: E402

# The eight controller-glyph atlases are also HD UI textures, but the Icons row hot-swaps them
# separately.  The icon column lets the HD UI scan find the right stock payload when a glyph set
# is in the live storage (its hash then differs from the base arc's).
BASE_ICONS = {
    "HD_custom_pnl2_BM_NOMIP": 1,
    "HD_window_BM_MQ_NOMIP": 2,
    "HD_lobby00_BM_NOMIP": 3,
    "HD_hud00_BM_NOMIP": 4,
    "button_festa_00_ID": 5,
}
LOC_ICONS = {
    "HD_custom_pnl2_BM_NOMIP": 6,
    "HD_window_BM_MQ_NOMIP": 7,
    "button_festa_00_ID": 8,
}


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def payload(data):
    if data[:4] != b"TEX\0" or len(data) < 0x18:
        return None
    size = struct.unpack_from("<I", data, 0x10)[0]
    if 0x18 + size > len(data):
        return None
    return data[0x18:0x18 + size]


def base_name(entry):
    return entry.split("\\")[-1]


def pairs(pp4, romfs, variant):
    """(hd arc, stock arc, icon table) for every arc the build merges."""
    v4 = os.path.join(pp4, "English", "HD UI v4", "romfs", "nativeNX", "eng", "arc")
    if os.path.isdir(v4):
        for root, _dirs, files in os.walk(v4):
            for name in sorted(files):
                if not name.endswith(".arc"):
                    continue
                rel = os.path.relpath(os.path.join(root, name), v4)
                yield os.path.join(v4, rel), os.path.join(romfs, "nativeNX", "eng", "arc", rel), {}
    layout = os.path.join(pp4, "English", "Layout")
    for name, context in (("resident_eng.arc", "base"), ("quest_eng.arc", "base")):
        hd = os.path.join(layout, variant, name)
        stock = os.path.join(romfs, "nativeNX", "eng", "arc", name)
        if os.path.isfile(hd) and os.path.isfile(stock):
            yield hd, stock, BASE_ICONS if context == "base" else {}
    top = variant.split("/")[0]
    loc = os.path.join(layout, top, "loc", "arc", "resident.arc")
    stock_loc = os.path.join(romfs, "nativeNX", "loc", "arc", "resident.arc")
    if os.path.isfile(loc) and os.path.isfile(stock_loc):
        yield loc, stock_loc, LOC_ICONS
    title = os.path.join(layout, top, "title_eng.arc")
    stock_title = os.path.join(romfs, "nativeNX", "eng", "arc", "title_eng.arc")
    if os.path.isfile(title) and os.path.isfile(stock_title):
        yield title, stock_title, {}


def main():
    if len(sys.argv) < 4 or len(sys.argv) > 5:
        raise SystemExit(__doc__)
    pp4, romfs, out = sys.argv[1], sys.argv[2], sys.argv[3]
    variant = sys.argv[4] if len(sys.argv) == 5 else "2-NS/HDI"
    if not os.path.isdir(os.path.join(pp4, "English")):
        raise SystemExit(f"pp4_ui: {pp4}: no English folder")
    if not os.path.isdir(os.path.join(romfs, "nativeNX")):
        raise SystemExit(f"pp4_ui: {romfs}: no nativeNX folder")
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        if name.endswith(".bin") or name == "ui.txt":
            os.unlink(os.path.join(out, name))

    seen = {}
    lines = []
    ntex = nsd = nskip = 0
    for hd_path, stock_path, icons in pairs(pp4, romfs, variant):
        try:
            hd = Arc.open(hd_path)
            stock = Arc.open(stock_path)
        except (OSError, ValueError) as e:
            print(f"pp4_ui: {hd_path}: {e}", file=sys.stderr)
            continue
        by_name = {e["name"]: e for e in stock.entries}
        for e in hd.entries:
            hd_data = hd.read(e)
            hp = payload(hd_data)
            if hp is None:
                continue
            se = by_name.get(e["name"])
            if not se:
                continue
            sp = payload(stock.read(se))
            if sp is None:
                continue
            ntex += 1
            if hp == sp:
                nskip += 1
                continue
            hh, sh = fnv1a64(hp), fnv1a64(sp)
            icon = icons.get(base_name(e["name"]), 0)
            key = (len(hp), hh, len(sp), sh, icon)
            if key in seen:
                continue
            ident = len(seen) + 1
            seen[key] = ident
            with open(os.path.join(out, f"{ident}.bin"), "wb") as f:
                f.write(sp)
            lines.append(f"{ident} {len(hp)} {hh:016x} {len(sp)} {sh:016x} {icon}")
            nsd += 1
    with open(os.path.join(out, "ui.txt"), "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))
    print(f"pp4_ui: {nsd} textures ({ntex} TEX entries, {nskip} identical)")
    total = 0
    for line in lines:
        total += int(line.split()[3])
    print(f"pp4_ui: stock payloads {total / 1e6:.1f} MB in {out}")


if __name__ == "__main__":
    main()
