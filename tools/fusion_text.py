"""Word the fusion screens for equipment, in the game's own Wycademy Lab text tables.

The native fusion screen's labels come from nativeNX/<lang>/arc/village/common_<lang>.arc, entry
<lang>/table/Lb_LaboratoryMsg_<lang>, a GMD v2.3 file. After its 0x28-byte header, a 16-byte name and
the key tables, the file ends with two pools: the key names (header +0x1c bytes) and the message texts
(header +0x20 bytes). The text pool is the NUL-terminated strings in message order, so rewording a
message only means replacing its string in the pool and updating the pool size - no offsets move.

This gives the fork's weapon fusion its "Fuse Equipment" / "Undo Equipment Fusion" wording from the
player's own dump, without another mod's romfs: the build runs it over build/data/romfs.

Usage: fusion_text.py <game romfs> <out romfs> [language ...]
  (default: every nativeNX/<lang>/arc/village/common_<lang>.arc in the game's romfs)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mtarc

# The English texts (the only language with known wording; other languages keep the game's own).
REPLACEMENTS = {
    # the action names and the smithy/undo headings
    b"Armor Fusion": b"Equipment Fusion",
    b"Fuse Armor": b"Fuse Equipment",
    b"Undo Armor Fusion": b"Undo Equipment Fusion",
    b"Fuse pieces of armor together\r\nto change how they look.":
        b"Fuse two pieces of equipment together\r\nto change how they look.",
    # the fusion box
    b"Choose a Base Armor.": b"Choose a Base Armor or Weapon.",
    b"Choose a Visual Armor.": b"Choose a Visual Armor or Weapon.",
    b"Choose a Base Armor.\r\n<COL FF0000FF>(You cannot fuse weapons.)</COL>":
        b"Choose a Base Armor or Weapon.\r\n<COL FF0000FF>(You cannot fuse Palico weapons.)</COL>",
    b"Choose a Visual Armor.\r\n<COL FF0000FF>(You cannot fuse weapons.)</COL>":
        b"Choose a Visual Armor or Weapon.\r\n<COL FF0000FF>(You cannot fuse Palico weapons.)</COL>",
    b"Choose a Base Armor.\r\n<COL FF0000FF>(You cannot fuse armor that\r\nhas already been fused.)</COL>":
        b"Choose a Base Armor or Weapon.\r\n<COL FF0000FF>(You cannot fuse equipment that\r\nhas already been fused.)</COL>",
    b"Choose a Visual Armor.\r\n<COL FF0000FF>(You cannot choose the base armor.)</COL>":
        b"Choose a Visual Armor or Weapon.\r\n<COL FF0000FF>(You cannot choose the base piece.)</COL>",
    b"Choose a Visual Armor.\r\n<COL FF0000FF>(You cannot choose the same\r\narmor as the base armor.)</COL>":
        b"Choose a Visual Armor or Weapon.\r\n<COL FF0000FF>(You cannot choose the same\r\npiece as the base.)</COL>",
    # the undo screen
    b"Choose the Armor Fusion you'd like to undo.":
        b"Choose the Equipment Fusion you'd like to undo.",
    b"Choose the Armor Fusion you'd like to undo.\r\n<COL FF0000FF>(The fused armor could not be removed\r\nbecause your Equipment Box is full.)</COL>":
        b"Choose the Equipment Fusion you'd like to undo.\r\n<COL FF0000FF>(The fused equipment could not be removed\r\nbecause your Equipment Box is full.)</COL>",
    b"Undo fusion for this armor?": b"Undo this fusion?",
    b"The separate armor pieces were\r\nsent to your Equipment Box.":
        b"The separate pieces were\r\nsent to your Equipment Box.",
    # the help lines under the two actions
    b"Fuse two pieces of armor to transfer\r\nthe appearance of one to the other.":
        b"Fuse two armor pieces or two weapons to transfer\r\nthe appearance of one to the other.",
    b"Undo the fusion process and split the\r\narmor into two separate pieces.":
        b"Undo the fusion process and split the\r\nequipment into two separate pieces.",
}


def patch_gmd(data):
    """The GMD with the fusion strings reworded. Returns (data, replaced strings)."""
    if data[:4] != b"GMD\0":
        raise ValueError("not a GMD file")
    text_size = struct.unpack_from("<I", data, 0x20)[0]
    start = len(data) - text_size
    if start <= 0x28:
        raise ValueError("text pool does not fit the file")
    strings = data[start:].split(b"\0")
    if not strings or strings[-1] != b"":
        raise ValueError("text pool is not NUL-terminated")
    out, replaced = [], []
    for s in strings:
        new = REPLACEMENTS.get(s, s)
        if new is not s:
            replaced.append((s, new))
        out.append(new)
    pool = b"\0".join(out)
    data = bytearray(data[:start]) + bytearray(pool)
    struct.pack_into("<I", data, 0x20, len(pool))
    return bytes(data), replaced


def patch_arc(path, out_path):
    arc = mtarc.Arc.open(path)
    files, replaced = [], []
    for e in arc.entries:
        data = arc.read(e)
        if "table\\Lb_LaboratoryMsg_" in e["name"] or "table/Lb_LaboratoryMsg_" in e["name"]:
            data, replaced = patch_gmd(data)
        files.append((e["name"], e["type"], data))
    if not replaced:
        return replaced
    blob = mtarc.build(files, version=arc.version)
    tmp = out_path + ".tmp"
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, out_path)  # a new file: the build's romfs entries may be hard links to the dump
    return replaced


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    romfs, out_romfs = argv[1], argv[2]
    # Default: the language folders that actually ship the village archive (nativeNX also has arc/, loc/, ...).
    languages = argv[3:] or sorted(
        d for d in os.listdir(os.path.join(romfs, "nativeNX"))
        if os.path.isfile(os.path.join(romfs, "nativeNX", d, "arc", "village", "common_%s.arc" % d)))
    failed = 0
    for lang in languages:
        rel = os.path.join("nativeNX", lang, "arc", "village", "common_%s.arc" % lang)
        src = os.path.join(romfs, rel)
        if not os.path.isfile(src):
            print("fusion text: %s: no such archive, skipped" % rel, file=sys.stderr)
            continue
        try:
            replaced = patch_arc(src, os.path.join(out_romfs, rel))
        except (OSError, ValueError) as e:
            print("fusion text: %s: %s" % (rel, e), file=sys.stderr)
            failed += 1
            continue
        if not replaced:
            print("fusion text: %s: not English text, kept as it is" % rel)
            continue
        print("fusion text: %s: %d string(s) reworded" % (rel, len(replaced)))
        for old, new in replaced:
            first = old.split(b"\r\n")[0].split(b"\n")[0]
            print("  %r -> %r" % (first[:60], new.split(b"\r\n")[0].split(b"\n")[0][:60]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
