"""Counts the shapes that decide whether emitted C reads as code or as assembly.

Not a quality score: a metric to compare two runs of the same sample. Labels and
gotos say how much structure the structurizer failed to recover; undef and
unnamed-value say how much the analyses could not resolve; the rest is volume.
"""

import re
import sys

PATTERNS = {
    "lines": None,
    "labels": re.compile(r"^L_0x[0-9a-f]+:", re.M),
    "gotos": re.compile(r"\bgoto L_0x", re.M),
    "switches": re.compile(r"^\s*switch \(", re.M),
    "cases": re.compile(r"^\s*case 0x", re.M),
    "loops": re.compile(r"^\s*(while \(|do \{)", re.M),
    "ifs": re.compile(r"^\s*(\} else )?if \(", re.M),
    "undef": re.compile(r"/\*undef\*/"),
    "unnamed": re.compile(r"/\*unnamed-value"),
    "dead-value": re.compile(r"/\*dead-value\*/"),
    "op-read": re.compile(r"/\* op read \*/"),
    # Catches ExprPrinter::inner's default case: any IL ExprOp the C emitter
    # has no switch arm for prints as this and silently becomes 0. Should
    # always read 0; a nonzero count means some ExprOp needs a case added.
    "unhandled-op": re.compile(r"/\*\w[\w.]*\?\*/0"),
    "flag-stub": re.compile(r"__xdec_flagcond_stub"),
    "intrinsics": re.compile(r"__xdec_intrin_"),
    "phi-snapshots": re.compile(r"__prev = "),
    "reg-vars": re.compile(r"not tracked by SSA"),
    "temps": re.compile(r"^\s+uint\d+_t t\d+;", re.M),
}


def measure(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    out = {"lines": text.count("\n")}
    for name, pattern in PATTERNS.items():
        if pattern is not None:
            out[name] = len(pattern.findall(text))
    return out


def main(paths):
    rows = [(path, measure(path)) for path in paths]
    keys = list(PATTERNS)
    width = max(len(key) for key in keys) + 2
    print("".ljust(width) + "".join(p.split("\\")[-1].rjust(22) for p, _ in rows))
    for key in keys:
        line = key.ljust(width)
        base = rows[0][1][key]
        for index, (_, values) in enumerate(rows):
            value = values[key]
            cell = str(value)
            if index > 0 and base != value:
                cell += f" ({value - base:+d})"
            line += cell.rjust(22)
        print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
