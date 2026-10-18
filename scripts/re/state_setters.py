#!/usr/bin/env python3
"""Pair rr6's and pgr3's render- and sampler-state setters by D3DRS/D3DSAMP
index (the {Get, Set, Default} tables D3D__InitializeStateTables walks) and
report how each one's device stores moved.

    state_setters.py <rr6 root> <rr6 xex> <pgr3 image> [csv out]

Both state enums are the API's, so index k is the same state in both XDKs.
For each pair the stores and loads off the device argument (r3 at entry,
usually moved to r31) are aligned by instruction shape and listed as
rr6 offset -> pgr3 offset.
"""
import csv
import difflib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from match_names import index, shape  # noqa: E402
from offset_map import mem_operand, strip  # noqa: E402

TABLES = {  # (rr6 VA, pgr3 VA, entries)
    "render": (0x823763E0, 0x825271F0, 97),
    "sampler": (0x82376870, 0x82527680, 20),
}


def read_table(image, base, va, n):
    o = va - base
    return [struct.unpack_from(">3I", image, o + 12 * i) for i in range(n)]


def main(argv):
    rr6_root, rr6_xex, pgr3_image = Path(argv[1]), Path(argv[2]), Path(argv[3])
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from extract_shaders import load_image  # noqa: E402
    rr6_img, rr6_base = load_image(rr6_xex.read_bytes())
    pgr3_img, pgr3_base = pgr3_image.read_bytes(), 0x82000000

    ref = index(rr6_root, ".funcdb_ref.pickle")["funcs"]
    own = index(Path(__file__).resolve().parents[2], ".funcdb.pickle")["funcs"]

    def func(db, va):
        return db.get(f"sub_{va:08X}") or next((f for n, f in db.items() if n.startswith("__imp__")
                                               and f.get("addr") == va), None)

    rows = []
    for kind, (rva, pva, n) in TABLES.items():
        rt = read_table(rr6_img, rr6_base, rva, n)
        pt = read_table(pgr3_img, pgr3_base, pva, n)
        for i in range(n):
            (rget, rset, rdef), (pget, pset, pdef) = rt[i], pt[i]
            rf, pf = func(ref, rset), func(own, pset)
            if not rf or not pf:
                rows.append([kind, i, f"0x{rset:08X}", f"0x{pset:08X}", "", "", "missing function"])
                continue
            ra, pa = strip(rf["asm"]), strip(pf["asm"])
            sm = difflib.SequenceMatcher(None, shape(rf["asm"]), shape(pf["asm"]), autojunk=False)
            moves = []
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag == "equal":
                    for k in range(i2 - i1):
                        a, b = mem_operand(ra[i1 + k]), mem_operand(pa[j1 + k])
                        if a and b and a[1] == b[1] and a[1] != 1 and a[0] >= 0x400:
                            moves.append((a[0], b[0], ra[i1 + k].split()[0]))
            rows.append([kind, i, f"0x{rset:08X}", f"0x{pset:08X}", f"{sm.ratio():.2f}",
                         f"0x{rdef:X}/0x{pdef:X}",
                         " ".join(f"{m}:{a:#06x}->{b:#06x}" for a, b, m in dict.fromkeys(moves))])
    out = csv.writer(open(argv[4], "w", newline="")) if len(argv) > 4 else None
    for r in rows:
        if out:
            out.writerow(r)
        print(f"{r[0]:7} {r[1]:3} {r[2]} {r[3]} {r[4]:>4} def {r[5]:14} {r[6]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
