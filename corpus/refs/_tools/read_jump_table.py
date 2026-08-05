#!/usr/bin/env python3
"""Read jump-table targets from ELF RELA addends (unrelocated .so on disk)."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def load_rela_addends(path: Path, va: int, count: int, width: int = 8) -> list[tuple[int, int]]:
    data = path.read_bytes()
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    end = va + count * width
    hits: dict[int, int] = {}
    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        sh_offset = struct.unpack_from("<Q", data, sh_off + 0x18)[0]
        sh_size = struct.unpack_from("<Q", data, sh_off + 0x20)[0]
        sh_entsize = struct.unpack_from("<Q", data, sh_off + 0x38)[0]
        if sh_entsize != 24 or sh_size == 0:
            continue
        for j in range(sh_size // sh_entsize):
            roff = sh_offset + j * sh_entsize
            r_offset, _r_info, r_addend = struct.unpack_from("<QQq", data, roff)
            if va <= r_offset < end:
                hits[r_offset] = r_addend
    out: list[tuple[int, int]] = []
    for i in range(count):
        slot_va = va + i * width
        if slot_va not in hits:
            raise SystemExit(f"missing reloc for table[{i}] @ 0x{slot_va:x}")
        out.append((i, hits[slot_va]))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("so", type=Path)
    ap.add_argument("va", type=lambda x: int(x, 0))
    ap.add_argument("--count", type=int, default=16)
    args = ap.parse_args()
    for idx, target in load_rela_addends(args.so, args.va, args.count):
        print(f"table[{idx:2d}] @ 0x{args.va + idx * 8:08x} -> 0x{target:08x}")


if __name__ == "__main__":
    main()
