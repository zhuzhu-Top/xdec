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
    # `case 0:` / `case 1:` with no hex value at all is the ordinal fallback
    # for a table-mode switch whose index was not fully reconstructable (see
    # analysis::matchDispatchValues) -- a switch a reader still has to
    # reverse-engineer back into the states it actually names.
    "ordinal-cases": re.compile(r"^\s*case \d+:", re.M),
    "loops": re.compile(r"^\s*(while \(|do \{)", re.M),
    "ifs": re.compile(r"^\s*(\} else )?if \(", re.M),
    # A promoted dispatcher state slot's own write -- distinct from every
    # other local, since this is the variable a flattening pass's whole
    # state machine turns on.
    "state-stores": re.compile(r"^\s*state = ", re.M),
    "undef": re.compile(r"/\*undef\*/"),
    "unnamed": re.compile(r"/\*unnamed-value"),
    "dead-value": re.compile(r"/\*dead-value\*/"),
    "op-read": re.compile(r"/\* op read \*/"),
    # Catches ExprPrinter::inner's default case: any IL ExprOp the C emitter
    # has no switch arm for prints as this and silently becomes 0. Should
    # always read 0; a nonzero count means some ExprOp needs a case added.
    "unhandled-op": re.compile(r"/\*\w[\w.]*\?\*/0"),
    # An opaque predicate's FlagCond that fold.cpp's chain could not resolve
    # (see passes/fold.cpp): the reader sees a bare `0`, not what the flags
    # actually said. Nonzero here is exactly Phase 2's flagcond target.
    "flagcond-stub": re.compile(r"/\*flagcond\*/0"),
    "intrinsics": re.compile(r"__xdec_intrin_"),
    # A dispatcher handler's shadow/live register hand-off through the
    # switch's shared tail (see analysis::LiveRegisterFrame) -- what Phase 4
    # (LiveRegisterFrame multi-cluster) exists to fold further.
    "dispatcher-relay-slots": re.compile(r"__prev = "),
    "reg-vars": re.compile(r"not tracked by SSA"),
    "temps": re.compile(r"^\s+uint\d+_t t\d+;", re.M),
    # A resolved jump-table index clamp printed as the plain ternary it
    # always was (see structure.cpp's collapseDispatchTree comment and
    # c_expr.cpp's Select case): `(bound < index) ? replacement : index`.
    # Not a quality signal by itself -- a dispatcher that recovers a real
    # N-way switch prints one of these per table read either way -- but a
    # count that drops to zero after the switch collapses into an unrelated
    # if-chain (or the reverse) says the discriminant moved somewhere the
    # dispatch-region view does not expect.
    "clamp-ternary": re.compile(
        r"\([a-z]+64_t\)\(0x[0-9a-f]+\) < \([a-z]+64_t\)\(\w+\)\) \? 0x[0-9a-f]+ : "),
    # The address arithmetic behind a jump-table read that survived to
    # emission as a raw dereference rather than a switch/case -- `(index <<
    # shift) + tableBase)`, the exact shape a resolved-but-unstructured
    # dispatch site leaves behind. Generic across any shift/base pair, not
    # tied to one sample's table address.
    "dispatch-load-sites": re.compile(r"<< 0x[0-9a-f]+\) \+ 0x[0-9a-f]+\)"),
}


def switchShapes(text):
    """Per-switch case counts, by scanning brace depth from each `switch (`
    to its matching close -- not a token a single regex can find, since
    nesting is exactly what tells one switch's cases from a nested one's.
    Returns (two_way_count, total_switch_count, while_true_count); the while
    count piggybacks on the same brace walk since both ask "what does this
    control block's body actually contain".
    """
    twoWay = 0
    total = 0
    whileTrue = 0
    index = 0
    while True:
        switchAt = text.find("switch (", index)
        whileAt = text.find("while (true)", index)
        if switchAt == -1 and whileAt == -1:
            break
        if whileAt != -1 and (switchAt == -1 or whileAt < switchAt):
            whileTrue += 1
            index = whileAt + len("while (true)")
            continue
        total += 1
        openBrace = text.find("{", switchAt)
        depth = 1
        cursor = openBrace + 1
        cases = 0
        while depth > 0 and cursor < len(text):
            char = text[cursor]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
            elif depth == 1 and text.startswith("case ", cursor) and (
                cursor == 0 or text[cursor - 1] in "\n\t "
            ):
                cases += 1
            cursor += 1
        if cases == 2:
            twoWay += 1
        # Past the token, not past the whole body: a switch nested inside
        # this one's case (or its epilogue) is still ahead of `index` and
        # still needs its own, independent pass.
        index = switchAt + len("switch (")
    return twoWay, total, whileTrue


_ROUTING_IF_OPENER = re.compile(r"if \(([^\n]*?)\) \{\n")


def duplicateRoutingIfs(text):
    """Counts condition strings that recur verbatim, once guarding a `state
    = ...` write and once guarding a `goto`/`continue` pair -- the same
    runtime decision spelled out to the reader twice under two different
    names (see structure.cpp's per-site dispatch handling and
    docs/00-core-vs-plugin-prompt.md's Phase 3a). Regex, not a brace parser:
    the window after each `if (` opener is generous (400 chars) but does not
    walk to the matching close, so this is a comparable-across-runs count,
    not an exact one -- the same spirit as `switchShapes` above.
    """
    seenKinds = {}
    for match in _ROUTING_IF_OPENER.finditer(text):
        condition = match.group(1)
        window = text[match.end():match.end() + 400]
        if re.search(r"^\s*state = ", window, re.M):
            seenKinds.setdefault(condition, set()).add("state")
        elif re.search(r"^\s*(goto |continue;)", window, re.M):
            seenKinds.setdefault(condition, set()).add("goto")
    return sum(1 for kinds in seenKinds.values() if len(kinds) == 2)


def measure(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    out = {"lines": text.count("\n")}
    for name, pattern in PATTERNS.items():
        if pattern is not None:
            out[name] = len(pattern.findall(text))
    twoWay, totalSwitches, whileTrue = switchShapes(text)
    out["two-way-switches"] = twoWay
    out["while-true"] = whileTrue
    out["duplicate-routing-if"] = duplicateRoutingIfs(text)
    # Not a token count: how much of the switch population is the shape a
    # plain `if`/`else` would say more plainly (see switchFor's collapse --
    # this stays nonzero only for the cases it does not reach, e.g. an index
    # still tied to a load).
    out["two-way-switch-ratio"] = f"{twoWay}/{totalSwitches or out['switches']}"
    return out


def main(paths):
    rows = [(path, measure(path)) for path in paths]
    keys = list(rows[0][1])  # measure()'s own insertion order, base plus derived
    width = max(len(key) for key in keys) + 2
    print("".ljust(width) + "".join(p.split("\\")[-1].rjust(22) for p, _ in rows))
    for key in keys:
        line = key.ljust(width)
        base = rows[0][1][key]
        for index, (_, values) in enumerate(rows):
            value = values[key]
            cell = str(value)
            if index > 0 and isinstance(value, int) and isinstance(base, int) and base != value:
                cell += f" ({value - base:+d})"
            line += cell.rjust(22)
        print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
