#!/usr/bin/env python3
"""Scan libscplugin .text for scatter-dispatcher epilogues (table ldr + br)."""
from __future__ import annotations

import re
import subprocess
import sys
from collections import defaultdict


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


def is_prologue_window(insns: list[tuple[int, str]], index: int) -> bool:
    """Heuristic: stp x29,x30 or sub sp at function start within 8 insns back."""
    for j in range(max(0, index - 8), index + 1):
        txt = insns[j][1]
        if re.search(r"\bstp\s+x29,\s*x30", txt) or re.search(r"\bsub\s+sp,\s+sp,", txt):
            return True
    return False


def find_dispatch_sites(insns: list[tuple[int, str]]) -> list[int]:
    """libscplugin scatter epilogue: cmp #0x2cc, str [*,#0xbe0], ldr [x25,*,lsl #3], br."""
    sites: list[int] = []
    for i, (addr, txt) in enumerate(insns):
        if not re.fullmatch(r"br\s+x\d+", txt):
            continue
        window = insns[max(0, i - 12) : i + 1]
        text = " | ".join(t for _, t in window)
        if not ("lsl #3" in text and "x25" in text and "#0x2cc" in text and "#0xbe0" in text):
            continue
        sites.append(addr)
    return sites


def cluster_into_functions(sites: list[int], gap: int = 0x400) -> list[tuple[int, int, int]]:
    """Return (start_addr, end_addr, site_count) clusters."""
    if not sites:
        return []
    sites = sorted(set(sites))
    clusters: list[list[int]] = [[sites[0]]]
    for addr in sites[1:]:
        if addr - clusters[-1][-1] <= gap:
            clusters[-1].append(addr)
        else:
            clusters.append([addr])
    result = []
    for cluster in clusters:
        start = cluster[0] & ~0xF
        # extend start backward to nearest prologue-ish boundary
        start = (cluster[0] // 0x100) * 0x100
        end = cluster[-1] + 0x40
        result.append((start, end, len(cluster)))
    return result


def main() -> None:
    so = sys.argv[1] if len(sys.argv) > 1 else r"E:\tmp\sc_apk\x\lib\arm64-v8a\libscplugin.so"
    text_start = 0x7DF30
    text_size = 0x1546F8
    text_stop = text_start + text_size

    chunk = 0x20000
    all_sites: list[int] = []
    for base in range(text_start, text_stop, chunk):
        stop = min(base + chunk, text_stop)
        insns = disasm_range(so, base, stop)
        all_sites.extend(find_dispatch_sites(insns))

    print(f"so: {so}")
    print(f".text: 0x{text_start:x}..0x{text_stop:x}")
    print(f"table_ldr_br sites: {len(all_sites)}")

    clusters = cluster_into_functions(all_sites)
    clusters.sort(key=lambda c: c[2], reverse=True)
    print(f"clusters (gap<=0x800): {len(clusters)}")
    print()
    print(f"{'sites':>6}  {'start':>10}  {'end':>10}  span")
    for start, end, count in clusters[:30]:
        print(f"{count:6d}  0x{start:08x}  0x{end:08x}  0x{end - start:x}")


if __name__ == "__main__":
    main()
