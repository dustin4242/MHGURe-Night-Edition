"""Build the weapon fusion catalog from a player's MHGU 1.4.0 dump.

Every weapon's fusion cost is the *monster* material of the weapon whose
appearance is being applied, so this reads the native weapon creation tables
(weaponCreateWxx.wcd) and the item tables to find, for each weapon, the
category the material picker should offer. The weapon's creation recipe names
its key material (flags byte 2), which the item data maps to a native material
category (itemCategoryTypeData.ict). The category's member items and their
native point values come from itemData.itm, exactly as the armour catalog's.

Weapons are rank-agnostic in this fork: a G-rank weapon may take an LR weapon's
appearance and vice versa, so entries carry no rank restriction.

No copyrighted tables are embedded; dumps/saves are read-only.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
from collections import defaultdict
from pathlib import Path

FUSION_POINTS = 10
FORMS = {
    "itemData.itm": (bytes.fromhex("6666aa41"), 44),
    "itemCategoryTypeData.ict": (bytes.fromhex("6666aa41"), 7),
}


def records(raw: bytes, structure: tuple[bytes, int], name: str) -> list[bytes]:
    version, stride = structure
    if len(raw) < 8 or raw[:4] != version:
        raise ValueError(f"{name}: unsupported or missing version header")
    count, = struct.unpack_from("<I", raw, 4)
    if count > 200_000 or len(raw) != 8 + count * stride:
        raise ValueError(f"{name}: count/record-size mismatch")
    return [raw[i:i + stride] for i in range(8, len(raw), stride)]


def item_tables(directory: Path):
    items = records((directory / "itemData.itm").read_bytes(), FORMS["itemData.itm"], "itemData.itm")
    categories = records((directory / "itemCategoryTypeData.ict").read_bytes(),
                         FORMS["itemCategoryTypeData.ict"], "itemCategoryTypeData.ict")
    by_id, members = {}, defaultdict(list)
    for row in items:
        item_id, = struct.unpack_from("<H", row)
        points = row[27]
        cats = sorted(set(struct.unpack_from("<8H", row, 28)) - {0})
        by_id[item_id] = (points, cats)
        for category in cats:
            if points:
                members[category].append({"item_id": item_id, "points": points})
    category_rank = {}
    for row in categories:
        category_id, = struct.unpack_from("<H", row)
        category_rank[category_id] = row[6]
    return by_id, category_rank, members


def weapon_creates(directory: Path):
    """(type id, weapon id) -> [(item, quantity, flags)], plus the parent link."""
    result = {}
    for path in sorted(directory.glob("weaponCreateW*.wcd")):
        number = int(path.stem[13:15])
        weapon_type = number + 7  # native equipment category: 7 = first weapon file
        raw = path.read_bytes()
        if len(raw) < 8 or raw[:4] != bytes.fromhex("0000803f"):
            raise ValueError(f"{path.name}: unsupported version")
        count, = struct.unpack_from("<I", raw, 4)
        if count > 10_000 or len(raw) != 8 + count * 38:
            raise ValueError(f"{path.name}: count/record-size mismatch")
        for i in range(count):
            row = raw[8 + i * 38: 8 + (i + 1) * 38]
            weapon_id, parent = struct.unpack_from("<HH", row, 2)
            materials = [struct.unpack_from("<HBB", row, 6 + 4 * k) for k in range(4)]
            materials = [m for m in materials if m[0]]
            # Some weapons have two recipes (an easier and a harder material
            # tier); both are candidates for the key material.
            result.setdefault((weapon_type, weapon_id), []).extend(materials)
    return result


def choose_material(materials, items):
    """The key (flags 2) material when it is usable, else the most valuable one."""
    usable = [(item, quantity, flags) for item, quantity, flags in materials
              if item in items and items[item][0] and items[item][1]]
    if not usable:
        return None
    key = [m for m in usable if m[2] == 2]
    if key:
        return key[0]
    return max(usable, key=lambda m: (items[m[0]][0], -m[0]))


def choose_category(item, items, category_rank):
    categories = items[item][1]
    # Categories are per monster and per tier; the highest tier one is the
    # weapon's native material. Ties keep the native order.
    return max(categories, key=lambda c: (category_rank.get(c, 0), -c))


def build(directory: Path) -> dict:
    items, category_rank, members = item_tables(directory)
    creates = weapon_creates(directory)
    entries, skipped = [], []
    for (weapon_type, weapon_id) in sorted(creates):
        materials = creates[(weapon_type, weapon_id)]
        material = choose_material(materials, items)
        if not material:
            skipped.append({"type": weapon_type, "id": weapon_id})
            continue
        item, quantity, flags = material
        category = choose_category(item, items, category_rank)
        entries.append({"type": weapon_type, "id": weapon_id,
                        "category_id": category, "points": FUSION_POINTS,
                        "material": {"item_id": item, "quantity": quantity, "flags": flags,
                                     "category_rank": category_rank.get(category, 0)}})
    used = sorted({e["category_id"] for e in entries})
    return {
        "schema": 1,
        "source_sha256": {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
                          for name in FORMS},
        "weapons": entries,
        "categories": [{"category_id": c, "material_rank": category_rank.get(c, 0),
                        "items": sorted(members[c], key=lambda x: x["item_id"])}
                       for c in used if members[c]],
        "skipped": skipped,
    }


def binary_catalog(result: dict) -> bytes:
    """WFUS v1: 48-byte header, 8-byte weapon entries, 8-byte members.

    Header <4sIII32s>: magic, version 1, weapon count, member count, digest.
    Weapon <BBHHBB>: type, rank (0: no restriction), id, category, points, reserved.
    Member <HHB3x>: category, item, unchanged native point value.
    """
    categories = {row["category_id"]: row for row in result["categories"]}
    entries, used = [], set()
    for row in result["weapons"]:
        category = row["category_id"]
        if category not in categories:
            raise ValueError(f"weapon {row['type']}/{row['id']}: unknown category {category}")
        used.add(category)
        entries.append(struct.pack("<BBHHBB", row["type"], 0, row["id"], category,
                                   row["points"], 0))
    materials = [struct.pack("<HHB3x", category, item["item_id"], item["points"])
                 for category in sorted(used) for item in categories[category]["items"]]
    digest = hashlib.sha256(b"".join(bytes.fromhex(value) for _, value in
                                   sorted(result["source_sha256"].items()))).digest()
    return b"WFUS" + struct.pack("<III", 1, len(entries), len(materials)) + digest + \
        b"".join(entries + materials)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("table", type=Path, help="romfs/nativeNX/table")
    parser.add_argument("--output", type=Path, help="audit JSON")
    parser.add_argument("--binary", type=Path, help="catalog file")
    args = parser.parse_args()
    result = build(args.table)
    if args.output:
        args.output.write_text(json.dumps(result, indent=1) + "\n")
    if args.binary:
        args.binary.write_bytes(binary_catalog(result))
    print(f"{len(result['weapons'])} weapons, {len(result['categories'])} categories, "
          f"{len(result['skipped'])} without a usable material")


if __name__ == "__main__":
    main()
