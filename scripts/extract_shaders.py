#!/usr/bin/env python3
"""Extract the Xenos shader containers embedded in PGR3's default.xex.

PGR3 (XDK 2.0.2099) ships every shader inside the XEX rather than as loose
.vso/.pso files, and its containers use the early 0x102A0E0x revision, as
RR6's do (this script is rr6's). The title update in default.xexp replaces 50
of the 305 shaders and rewrites the padding of most others, so the build reads
the image as the SDK loads it, patch applied, from pgr3_dump_image (--image).
Reading the XEX itself (decrypt and decompress) gives the unpatched set and is
kept for comparison. It scans the image for containers and writes one file
per unique container, which XenosRecomp then consumes:

    <out_dir>/vs_<VA>.vso    vertex shaders   (flags & 1 == 1)
    <out_dir>/ps_<VA>.pso    pixel shaders    (flags & 1 == 0)

<VA> is the guest address of the first copy of that container, so a name can
be looked up directly in a disassembler. Duplicate containers (same bytes at
several addresses) are written once and their extra addresses listed in
shaders.csv alongside the first.

The build runs this into out/build/<preset>/generated/shaders (see
CMakeLists.txt). Run by hand, it defaults to out/shaders.

Usage: extract_shaders.py --image <image> <xex> [out_dir]   (pgr3_dump_image output)
       extract_shaders.py [xex] [out_dir]             (unpatched; needs cryptography)
"""
import csv
import struct
import sys
from pathlib import Path

XEX_RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
XEX_DEVKIT_KEY = bytes(16)
OPT_FILE_FORMAT_INFO = 0x000003FF
OPT_IMAGE_BASE_ADDRESS = 0x00010201

CONTAINER_REV_0E = 0x102A0E00
CTAB_HEADER_SIZE = 0x1C
CTAB_VERSION_VS = 0xFFFE0300
CTAB_VERSION_PS = 0xFFFF0300
MAX_CONTAINER_SIZE = 0x20000


def load_image(xex: bytes) -> tuple[bytes, int]:
    magic, _flags, pe_offset, _res, sec_offset, header_count = struct.unpack_from(
        ">4sIIIII", xex, 0)
    if magic != b"XEX2":
        raise SystemExit("not an XEX2 file")

    headers = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(header_count))
    image_base = headers.get(OPT_IMAGE_BASE_ADDRESS)
    if image_base is None:
        # Without the optional header the base lives in the security info.
        image_base = struct.unpack_from(">I", xex, sec_offset + 0x110)[0]

    ffi = headers[OPT_FILE_FORMAT_INFO]
    info_size, encryption, compression = struct.unpack_from(">IHH", xex, ffi)
    if compression != 1:
        raise SystemExit(f"unsupported XEX compression type {compression} (only basic)")
    blocks = [struct.unpack_from(">II", xex, ffi + 8 + i * 8)
              for i in range((info_size - 8) // 8)]

    payload = xex[pe_offset:]
    if encryption:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

        payload = payload[: len(payload) // 16 * 16]
        file_key = xex[sec_offset + 0x150: sec_offset + 0x160]
        for key in (XEX_RETAIL_KEY, XEX_DEVKIT_KEY):
            session = Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(file_key)
            plain = Cipher(algorithms.AES(session), modes.CBC(bytes(16))).decryptor().update(payload)
            if plain[:2] == b"MZ":
                payload = plain
                break
        else:
            raise SystemExit("XEX decryption failed with both retail and devkit keys")

    image = bytearray()
    pos = 0
    for data_size, zero_size in blocks:
        image += payload[pos: pos + data_size]
        image += bytes(zero_size)
        pos += data_size
    if image[:2] != b"MZ":
        raise SystemExit("decompressed image has no PE header")
    return bytes(image), image_base


def xex_image_base(xex: bytes) -> int:
    """The load address from the XEX's (unencrypted) optional headers."""
    magic, _flags, _pe, _res, sec_offset, header_count = struct.unpack_from(
        ">4sIIIII", xex, 0)
    if magic != b"XEX2":
        raise SystemExit("not an XEX2 file")
    headers = dict(struct.unpack_from(">II", xex, 24 + i * 8) for i in range(header_count))
    base = headers.get(OPT_IMAGE_BASE_ADDRESS)
    return base if base is not None else struct.unpack_from(">I", xex, sec_offset + 0x110)[0]


def load_loaded_image(data: bytes, xex: bytes) -> tuple[bytes, int]:
    """A pgr3_dump_image image. Its PE header's ImageBase is the PE default,
    not the load address, so the base comes from the XEX header."""
    if data[:2] != b"MZ":
        raise SystemExit("image has no PE header")
    return data, xex_image_base(xex)


def scan_containers(image: bytes):
    """Yields (offset, size, is_vertex) for every rev 0x0E container."""
    for i in range(0, len(image) - 0x18, 4):
        flags, virtual_size, physical_size, _c, ctab_offset, _d = struct.unpack_from(
            ">6I", image, i)
        if flags & 0xFFFFFFFE != CONTAINER_REV_0E:
            continue
        size = virtual_size + physical_size
        if not 0 < size <= MAX_CONTAINER_SIZE or i + size > len(image):
            continue
        if ctab_offset + 0x14 > virtual_size:
            continue
        _ctab_size, ctab_header, _creator, version = struct.unpack_from(
            ">4I", image, i + ctab_offset)
        is_vertex = bool(flags & 1)
        if ctab_header != CTAB_HEADER_SIZE:
            continue
        if version != (CTAB_VERSION_VS if is_vertex else CTAB_VERSION_PS):
            continue
        yield i, size, is_vertex


STACK_BASE = 0x7F000000  # nominal r1 at entry for the stack-built containers
MFLR_R12 = 0x7D8802A6
BLR = 0x4E800020


def stack_built_containers(image: bytes, image_base: int):
    """Yields (call_va, function_va, blob) for containers the game builds on the
    stack from instruction immediates, which a byte scan cannot see.

    PGR3 has four: sub_821545A8 and sub_821DA180 store a VS and a PS word by
    word (li/lis/ori into registers, stw to r1+N) and pass r1+N to
    D3DDevice_Create*Shader or a wrapper. Every function that materialises the
    container magic (lis rX,0x102A) is replayed from its mflr r12 prologue:
    constant-building instructions are evaluated, stores go into a model of
    the stack, conditional branches fall through, and at each call whose r3 or
    r5 points at a complete container on the model stack, the container is
    taken. A container with any byte not produced by a constant is rejected.
    """
    words = len(image) // 4
    starts = set()
    for i in range(words):
        w = struct.unpack_from(">I", image, i * 4)[0]
        if w & 0xFC1FFFFF != 0x3C00102A:  # lis rX,0x102A
            continue
        for j in range(i, max(i - 0x1000, 0), -1):
            if struct.unpack_from(">I", image, j * 4)[0] == MFLR_R12:
                starts.add(j * 4)
                break

    for start in sorted(starts):
        regs = {1: STACK_BASE}
        mem = {}
        found = []

        def store(addr, value, n):
            if addr is None:
                return
            for k in range(n):
                mem[addr + k] = None if value is None else (value >> (8 * (n - 1 - k))) & 0xFF

        def take(ptr):
            if ptr is None or not STACK_BASE - 0x10000 <= ptr < STACK_BASE:
                return None
            head = [mem.get(ptr + k) for k in range(12)]
            if None in head:
                return None
            flags, vsz, psz = struct.unpack(">3I", bytes(head))
            if flags & 0xFFFFFFFE != CONTAINER_REV_0E or not 0 < vsz + psz <= MAX_CONTAINER_SIZE:
                return None
            body = [mem.get(ptr + k) for k in range(vsz + psz)]
            if None in body:
                raise SystemExit(f"stack-built container at call in 0x{image_base + start:08X} "
                                 "has bytes not built from constants")
            return bytes(body)

        pc = start
        for _ in range(0x4000):
            w = struct.unpack_from(">I", image, pc)[0]
            op = w >> 26
            d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            simm = (w & 0xFFFF) - ((w & 0x8000) << 1)
            uimm = w & 0xFFFF
            ra = regs.get(a) if a else 0
            if w == BLR:
                break
            if op == 14:    # addi / li
                regs[d] = None if ra is None else (ra + simm) & 0xFFFFFFFF
            elif op == 15:  # addis / lis
                regs[d] = None if ra is None else (ra + (simm << 16)) & 0xFFFFFFFF
            elif op in (24, 25):  # ori / oris
                rs = regs.get(d)
                regs[a] = None if rs is None else rs | (uimm << (16 if op == 25 else 0))
            elif op in (36, 37, 38, 44):  # stw, stwu, stb, sth
                addr = None if ra is None else (ra + simm) & 0xFFFFFFFF
                store(addr, regs.get(d), {36: 4, 37: 4, 38: 1, 44: 2}[op])
                if op == 37:
                    regs[a] = addr
            elif op == 31 and (w >> 1) & 0x3FF == 444:  # or / mr
                rs, rb = regs.get(d), regs.get(b)
                regs[a] = None if rs is None or rb is None else rs | rb
            elif op == 18:  # b / bl
                if not w & 1:
                    break   # a tail branch ends the function
                for r in (3, 5):  # Create*Shader(r3), or the wrappers' (.., .., r5)
                    blob = take(regs.get(r))
                    if blob is not None:
                        found.append((image_base + pc, blob))
                for r in (0, *range(3, 13)):
                    regs[r] = None
            elif op in (10, 11, 16, 19) or (op == 31 and (w >> 1) & 0x3FF in (0, 32)):
                pass        # compares and conditional branches: fall through
            elif op in range(32, 36) or op in range(40, 44) or op in (46, 58):
                regs[d] = None  # loads
            elif op in (20, 21, 23, 28, 29, 31):
                regs[a] = None  # rlwinm/andi./logical: rA is written
                if op == 31:
                    regs[d] = None
            pc += 4
        for call_va, blob in found:
            yield call_va, image_base + start, blob


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    args = sys.argv[1:]
    if args and args[0] == "--image":
        if len(args) < 3:
            raise SystemExit("--image needs the image and the XEX it was loaded from")
        image, image_base = load_loaded_image(Path(args[1]).read_bytes(),
                                              Path(args[2]).read_bytes())
        args = args[3:]
    else:
        xex_path = Path(args.pop(0)) if args else root / "assets" / "default.xex"
        image, image_base = load_image(xex_path.read_bytes())
    out_dir = Path(args[0]) if args else root / "out" / "shaders"
    out_dir.mkdir(parents=True, exist_ok=True)

    unique: dict[bytes, dict] = {}
    for offset, size, is_vertex in scan_containers(image):
        blob = image[offset: offset + size]
        va = image_base + offset
        entry = unique.get(blob)
        if entry is None:
            unique[blob] = {"va": va, "vertex": is_vertex, "size": size, "aliases": []}
        else:
            entry["aliases"].append(va)

    for entry in unique.values():
        entry["origin"] = "image"
    stack_built = 0
    for call_va, func_va, blob in stack_built_containers(image, image_base):
        if blob in unique:
            continue
        stack_built += 1
        unique[blob] = {"va": call_va, "vertex": bool(blob[3] & 1), "size": len(blob),
                        "aliases": [], "origin": f"stack:sub_{func_va:08X}"}

    for path in list(out_dir.glob("vs_*.vso")) + list(out_dir.glob("ps_*.pso")):
        path.unlink()

    rows = []
    for blob, e in sorted(unique.items(), key=lambda kv: kv[1]["va"]):
        name = f"{'vs' if e['vertex'] else 'ps'}_{e['va']:08X}.{'vso' if e['vertex'] else 'pso'}"
        (out_dir / name).write_bytes(blob)
        rows.append([name, f"0x{e['va']:08X}", e["size"],
                     " ".join(f"0x{a:08X}" for a in e["aliases"]), e["origin"]])

    with open(out_dir / "shaders.csv", "w", newline="") as f:
        w = csv.writer(f)
        # va: the first copy's address, or for a stack-built container the
        # call that creates it.
        w.writerow(["file", "va", "size", "alias_vas", "origin"])
        w.writerows(rows)

    vs = sum(1 for e in unique.values() if e["vertex"])
    refs = sum(1 + len(e["aliases"]) for e in unique.values())
    print(f"{len(unique)} unique shaders ({vs} vs, {len(unique) - vs} ps) "
          f"from {refs} containers, {stack_built} of them stack-built -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
