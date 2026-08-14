#!/usr/bin/env python3
"""Find smaller functions with table-dispatch epilogues (relaxed patterns)."""
from __future__ import annotations

import re
import subprocess
import sys


def disasm_range(so: str, start: int, stop: int) -> list[tuple[int, str]]:
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


def find_sites(insns: list[tuple[int, str]], require_2cc: bool) -> list[int]:
    sites: list[int] = []
    for i, (addr, txt) in enumerate(insns):
        if not re.fullmatch(r"br\s+x\d+", txt):
            continue
        window = insns[max(0, i - 10) : i + 1]
        text = " | ".join(t for _, t in window)
        if "lsl #3" not in text or "ldr" not in text:
            continue
        if require_2cc and "#0x2cc" not in text:
            continue
        sites.append(addr)
    return sites


def cluster(sites: list[int], gap: int) -> list[tuple[int, int, int]]:
    if not sites:
        return []
    sites = sorted(set(sites))
    clusters: list[list[int]] = [[sites[0]]]
    for addr in sites[1:]:
        if addr - clusters[-1][-1] <= gap:
            clusters[-1].append(addr)
        else:
            clusters.append([addr])
    out = []
    for c in clusters:
        span = c[-1] - c[0]
        out.append((c[0], c[-1], len(c), span))
    return out


def main() -> None:
    so = sys.argv[1] if len(sys.argv) > 1 else r"E:\tmp\sc_apk\x\lib\arm64-v8a\libscplugin.so"
    text_start, text_stop = 0x7DF30, 0x7DF30 + 0x1546F8
    chunk = 0x20000
    for label, require_2cc in [("clamp_2cc+table", True), ("any_table_ldr_br", False)]:
        all_sites: list[int] = []
        for base in range(text_start, text_stop, chunk):
            insns = disasm_range(so, base, min(base + chunk, text_stop))
            all_sites.extend(find_sites(insns, require_2cc))
        print(f"\n=== {label}: {len(all_sites)} sites ===")
        for gap in (0x300, 0x800, 0x2000):
            clusters = cluster(all_sites, gap)
            small = [c for c in clusters if c[3] <= 0x3000 and c[2] >= 3]
            small.sort(key=lambda c: (c[3], -c[2]))
            print(f"  gap=0x{gap:x}: {len(clusters)} clusters, {len(small)} with span<=0x3000 and sites>=3")
            for start, end, count, span in small[:15]:
                print(f"    sites={count:4d}  0x{start:08x}..0x{end:08x}  span=0x{span:x}")


if __name__ == "__main__":
    main()
