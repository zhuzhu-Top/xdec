#!/usr/bin/env python3
"""Classify dispatch epilogues in libscplugin function from objdump."""
from __future__ import annotations

import re
import subprocess
import sys
from collections import Counter


def parse_objdump(start: int, stop: int, so: str) -> list[tuple[int, str]]:
    out = subprocess.check_output(
        ["llvm-objdump", "-d", f"--start-address=0x{start:x}", f"--stop-address=0x{stop:x}", so],
        text=True,
        errors="replace",
    )
    insns: list[tuple[int, str]] = []
    for ln in out.splitlines():
        m = re.match(r"\s+([0-9a-f]+):\s+[0-9a-f ]+\s+(.+)", ln)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip()))
    return insns


def main() -> None:
    so = sys.argv[1] if len(sys.argv) > 1 else r"E:\tmp\sc_apk\x\lib\arm64-v8a\libscplugin.so"
    start, stop = 0x1164F8, 0x1226C4
    insns = parse_objdump(start, stop, so)

    # Full epilogue: str [x24,#0xbe0] within 6 insns + ldr [x25,...lsl #3] + br xN
    epilogues: list[int] = []
    for i, (addr, txt) in enumerate(insns):
        if not re.fullmatch(r"br\s+x\d+", txt):
            continue
        window = insns[max(0, i - 6) : i + 1]
        text = " | ".join(t for _, t in window)
        if "x25" in text and "lsl #3" in text and "0xbe0" in text:
            epilogues.append(addr)

    # Table adr
    table_adr = [(a, t) for a, t in insns if "0x1e70a0" in t]

    # br x after ldr x25 without state store (variant)
    table_br = []
    for i, (addr, txt) in enumerate(insns):
        if re.fullmatch(r"br\s+x\d+", txt):
            prev = insns[i - 1][1] if i else ""
            if "x25" in prev and "lsl #3" in prev:
                table_br.append(addr)

    # csel immediately before cmp 0x2cc (binary state pick)
    binary_pick = 0
    for i in range(len(insns) - 3):
        a0, t0 = insns[i]
        if not re.search(r"csel\s+x\d+", t0):
            continue
        ahead = " ".join(t for _, t in insns[i : i + 4])
        if "#0x2cc" in ahead and "x25" in " ".join(t for _, t in insns[i : i + 10]):
            binary_pick += 1

    print(f"range: 0x{start:x}..0x{stop:x}")
    print(f"instructions: {len(insns)}")
    print(f"table_adr_0x1e70a0: {len(table_adr)}")
    print(f"table_ldr_br (any state store within -6): {len(epilogues)}")
    print(f"table_ldr_br (immediate pair): {len(table_br)}")
    print(f"binary csel->clamp->table windows: {binary_pick}")

    # br x register hist for table_br
    regs = Counter(re.search(r"br\s+(x\d+)", insns[i][1]).group(1) for i, addr in enumerate(insns) if addr in table_br for _ in [0])
    # fix: match by addr
    reg_hist: Counter[str] = Counter()
    for i, (addr, txt) in enumerate(insns):
        if addr not in table_br:
            continue
        m = re.search(r"br\s+(x\d+)", txt)
        if m:
            reg_hist[m.group(1)] += 1
    print(f"br register after table load: {dict(reg_hist)}")

    # Non-x25 lsl#3 table loads
    other = [(a, t) for a, t in insns if re.search(r"ldr\s+x\d+,\s+\[x\d+,\s+x\d+, lsl #3\]", t) and "x25" not in t]
    print(f"other_lsl3_table_loads: {len(other)}")


if __name__ == "__main__":
    main()
