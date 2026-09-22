#!/usr/bin/env python3
"""Deeper goto categorization for decompiled scatter-dispatcher output."""
import re
import sys
from collections import Counter, defaultdict

def main(path: str) -> None:
    lines = open(path, encoding="utf-8").read().splitlines()

    # Map label -> definition line and body snippet
    label_def = {}
    for i, line in enumerate(lines, 1):
        m = re.match(r"^L_0x([0-9a-f]+):", line)
        if m:
            label_def[m.group(1)] = i

    gotos = []
    for i, line in enumerate(lines, 1):
        m = re.search(r"goto L_0x([0-9a-f]+);", line)
        if m:
            gotos.append((i, m.group(1), line.strip()))

    ref = Counter(t for _, t, _ in gotos)

    # For each single-ref goto, classify why it might remain
    single = [(i, t, line) for i, t, line in gotos if ref[t] == 1]

    def context(i: int) -> dict:
        window = lines[max(0, i - 15): min(len(lines), i + 8)]
        text = "\n".join(window)
        return {
            "in_case": bool(re.search(r"case 0x", text)),
            "in_while": "while (true)" in text,
            "has_switch_after_label": False,
            "label_has_switch": False,
            "label_has_nested_switch": False,
        }

    cats = Counter()
    samples = defaultdict(list)

    for i, t, line in single:
        ctx = context(i)
        def_i = label_def.get(t)
        if def_i:
            body = "\n".join(lines[def_i:def_i + 25])
            ctx["label_has_switch"] = "switch (" in body
            ctx["label_has_nested_switch"] = body.count("switch (") >= 2

        if "case 0x" in line:
            cat = "case_inline_goto"
        elif re.search(r"case 0x", lines[i - 2] if i >= 2 else ""):
            cat = "case_next_line_goto"
        elif ctx["in_while"] and ctx["label_has_switch"]:
            cat = "loop_remnant_with_dispatch"
        elif ctx["label_has_nested_switch"]:
            cat = "target_is_nested_dispatch"
        elif ctx["label_has_switch"]:
            cat = "target_is_dispatch"
        elif ctx["in_while"]:
            cat = "loop_body_goto"
        else:
            cat = "forward_or_hub"

        cats[cat] += 1
        if len(samples[cat]) < 2:
            samples[cat].append((i, t, line[:70]))

    print("=== Remaining single-ref gotos (J2g candidates?) ===")
    print(f"count: {len(single)}")
    for cat, n in cats.most_common():
        print(f"  {cat}: {n}")
        for s in samples[cat]:
            print(f"    line {s[0]} -> L_0x{s[1]}: {s[2]}")

    print("\n=== Multi-ref goto distribution ===")
    dist = Counter(ref.values())
    for k in sorted(dist):
        print(f"  {k} refs: {dist[k]} targets, {k * dist[k]} gotos")

    # case-only gotos still present
    case_only = 0
    for i, line in enumerate(lines):
        if re.search(r"case 0x.*goto L_", line):
            case_only += 1
        elif re.search(r"case 0x", line) and i + 1 < len(lines) and "goto L_" in lines[i + 1]:
            case_only += 1
    print(f"\ncase slots still bare goto (approx): {case_only}")

    # fallthrough label used as local anchor (goto to label defined inside same switch case)
    internal = 0
    for i, t, _ in gotos:
        def_i = label_def.get(t)
        if def_i and abs(def_i - i) < 30 and def_i > i:
            internal += 1
    print(f"goto to nearby label (same case fallthrough, <30 lines): {internal}")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "samples/build/out/sample_libscplugin.c")
