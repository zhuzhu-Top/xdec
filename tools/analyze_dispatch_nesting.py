#!/usr/bin/env python3
"""Analyze nested dispatch (scatter-dispatcher) structure from xdec IL dump.

Usage:
  xdec decompile ... --dump-il -o nul > il.txt
  python tools/analyze_dispatch_nesting.py il.txt

Evidence-based metrics for libscplugin-class flattening (not heuristic guesses).
"""
from __future__ import annotations

import re
import sys
from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


@dataclass
class Block:
    id: int
    va: int
    end_va: int
    preds: list[int]
    succs: list[int] = field(default_factory=list)
    has_brind: bool = False
    brind_targets: list[int] = field(default_factory=list)
    op_count: int = 0


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16")
    return raw.decode("utf-8", errors="replace")


def parse_il(text: str) -> dict[int, Block]:
    blocks: dict[int, Block] = {}
    current: Block | None = None
    for line in text.splitlines():
        m = re.match(r"\s*block b(\d+) @0x([0-9a-f]+)(?:\.\.0x([0-9a-f]+))?.*preds=\[(.*?)\]", line)
        if m:
            bid = int(m.group(1))
            va = int(m.group(2), 16)
            end_va = int(m.group(3), 16) if m.group(3) else va
            preds = [int(x.strip()[1:]) for x in m.group(4).split(",") if x.strip().startswith("b")]
            current = Block(id=bid, va=va, end_va=end_va, preds=preds)
            blocks[bid] = current
            continue
        if current is None:
            continue
        if line.strip() in ("}", "}"):
            current = None
            continue
        if "brind" in line:
            current.has_brind = True
            tm = re.search(r"-> \[(.*?)\]", line)
            if tm:
                current.brind_targets = [int(x.strip()[1:]) for x in tm.group(1).split(",") if x.strip().startswith("b")]
            current.succs = list(current.brind_targets)
            continue
        bm = re.match(r"\s*br b(\d+)", line)
        if bm:
            tgt = int(bm.group(1))
            current.succs = [tgt]
            continue
        cm = re.match(r"\s*condbr .* -> b(\d+), b(\d+)", line)
        if cm:
            current.succs = [int(cm.group(1)), int(cm.group(2))]
            continue
        if re.match(r"\s*(return|unreachable)", line):
            current.succs = []
            continue
        if re.match(r"\s*(store|load|%|call|intrinsic|nop|@)", line):
            current.op_count += 1
    return blocks


def dispatch_sites(blocks: dict[int, Block]) -> list[int]:
    return sorted(bid for bid, b in blocks.items() if b.has_brind)


def first_dispatch_reachable(
    blocks: dict[int, Block], start: int, dispatch_set: set[int], limit: int = 48
) -> int | None:
    """First dispatch block reached from a handler entry block (not the site itself)."""
    if start in dispatch_set:
        return None
    seen: set[int] = set()
    q: deque[tuple[int, int]] = deque([(start, 0)])
    while q:
        node, depth = q.popleft()
        if node in seen:
            continue
        seen.add(node)
        if depth > limit:
            continue
        blk = blocks.get(node)
        if blk is None:
            continue
        for succ in blk.succs:
            if succ in dispatch_set:
                return succ
            q.append((succ, depth + 1))
    return None


def build_dispatch_dag(blocks: dict[int, Block], sites: list[int]) -> dict[int, set[int]]:
    dset = set(sites)
    dag: dict[int, set[int]] = {s: set() for s in sites}
    for site in sites:
        b = blocks[site]
        for tgt in b.brind_targets:
            nxt = first_dispatch_reachable(blocks, tgt, dset)
            if nxt is not None and nxt != site:
                dag[site].add(nxt)
    return dag


def max_depth(dag: dict[int, set[int]], roots: Iterable[int]) -> int:
    memo: dict[int, int] = {}

    def depth(node: int, stack: set[int]) -> int:
        if node in stack:
            return 0
        if node in memo:
            return memo[node]
        stack.add(node)
        children = dag.get(node, ())
        if not children:
            d = 0
        else:
            d = 1 + max(depth(c, stack) for c in children)
        stack.remove(node)
        memo[node] = d
        return d

    return max((depth(r, set()) for r in roots), default=0)


def find_roots(dag: dict[int, set[int]], all_sites: set[int]) -> list[int]:
    referenced = {c for outs in dag.values() for c in outs}
    return sorted(all_sites - referenced)


def analyze(path: Path) -> None:
    blocks = parse_il(read_text(path))
    sites = dispatch_sites(blocks)
    site_set = set(sites)
    dag = build_dispatch_dag(blocks, sites)

    # Site out-degree within dispatch DAG (nested child dispatches)
    child_counts = Counter(len(dag[s]) for s in sites)
    # How many sites are targets of another site's handler path
    referenced_as_child = Counter()
    for site, children in dag.items():
        for c in children:
            referenced_as_child[c] += 1

    roots = find_roots(dag, site_set)
    mdepth = max_depth(dag, roots if roots else sites)

    print(f"file: {path}")
    print(f"blocks: {len(blocks)}")
    print(f"dispatch sites (brind): {len(sites)}")
    print(f"dispatch DAG roots (never child of another site path): {len(roots)}")
    print(f"max nested dispatch depth (CFG): {mdepth}")
    print(f"child dispatch count per site: {dict(sorted(child_counts.items()))}")
    print(f"sites referenced as nested child: {sum(1 for s in sites if referenced_as_child[s])}")
    print(f"2-target sites: {sum(1 for s in sites if len(blocks[s].brind_targets)==2)}")
    print(f"3+ target sites: {sum(1 for s in sites if len(blocks[s].brind_targets)>=3)}")

    # Example chain around 0x117164 region
    va_to_site = {blocks[s].va: s for s in sites}
    for va in [0x116588, 0x116624, 0x117164, 0x121b84, 0x117ac0]:
        if va in va_to_site:
            s = va_to_site[va]
            kids = [blocks[k].va for k in dag[s]]
            print(f"  site @0x{va:x} (b{s}) -> nested children @ {[hex(x) for x in kids]}")

    # Longest chain sample
    best: list[int] = []

    def walk(node: int, path: list[int], seen: set[int]) -> None:
        nonlocal best
        if node in seen:
            return
        path = path + [node]
        if len(path) > len(best):
            best = path
        for c in dag.get(node, ()):
            walk(c, path, seen | {node})

    for r in roots[:20]:
        walk(r, [], set())
    if best:
        chain = " -> ".join(f"0x{blocks[s].va:x}" for s in best)
        print(f"longest dispatch chain sample ({len(best)} hops): {chain}")


if __name__ == "__main__":
    analyze(Path(sys.argv[1] if len(sys.argv) > 1 else "samples/build/il_libscplugin.txt"))
