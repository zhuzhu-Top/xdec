#!/usr/bin/env python3
"""Quick goto pattern analysis for decompiled C output."""
import re
import sys
from collections import Counter

def main(path: str) -> None:
    text = open(path, encoding="utf-8").read().splitlines()

    gotos = []
    labels = set()
    for i, line in enumerate(text, 1):
        m = re.search(r"^L_0x([0-9a-f]+):", line)
        if m:
            labels.add(m.group(1))
        m = re.search(r"goto L_0x([0-9a-f]+);", line)
        if m:
            ctx = "plain"
            for j in range(max(0, i - 6), i):
                if re.search(r"case 0x", text[j]):
                    ctx = "case_goto"
                    break
            gotos.append((i, m.group(1), ctx, line.strip()))

    ref = Counter(t for _, t, _, _ in gotos)
    case_gotos = sum(1 for _, _, c, _ in gotos if c == "case_goto")
    other_gotos = len(gotos) - case_gotos

    print(f"file: {path}")
    print(f"total gotos: {len(gotos)}")
    print(f"total labels: {len(labels)}")
    print(f"unique targets: {len(ref)}")
    print(f"  single-ref targets: {sum(1 for c in ref.values() if c == 1)} "
          f"(gotos={sum(c for t,c in ref.items() if c==1)})")
    print(f"  multi-ref targets: {sum(1 for c in ref.values() if c > 1)} "
          f"(gotos={sum(c for t,c in ref.items() if c>1)})")
    print(f"case-associated gotos: {case_gotos}")
    print(f"other gotos: {other_gotos}")

    print("\nTop 15 merge hubs:")
    for t, c in ref.most_common(15):
        print(f"  L_0x{t}: {c} refs")

    continues = sum(1 for l in text if re.search(r"\bcontinue;", l))
    print(f"\ncontinue statements: {continues}")

    # Labels with no incoming goto (fallthrough only?)
    referenced = set(ref.keys())
    unreferenced_labels = labels - referenced
    print(f"labels never targeted by goto: {len(unreferenced_labels)}")

    # Sample a multi-ref hub region
    if ref:
        hub = ref.most_common(1)[0][0]
        print(f"\nSample hub L_0x{hub} refs at lines:")
        for i, t, ctx, line in gotos:
            if t == hub:
                print(f"  {i}: [{ctx}] {line[:80]}")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "samples/build/out/sample_libscplugin.c")
