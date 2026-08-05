#!/usr/bin/env python3
"""Score decompiled C against the ground truth the corpus was built from.

Every case in the manifest names a function whose original C source is known, so
a failure here is a real defect rather than a matter of taste. What is checked is
kept to properties that survive compilation: how much structure was recovered
(gotos, loops, switches), whether parameter widths and pointer-ness came back,
and whether the emitted blocks are in an order a reader can follow. Signedness
and variable names are not checked, because the machine code does not always
carry them and demanding a guess would reward guessing.

That last rule is what the modes are about. With `--types` the decompiler is
*told* the signedness and the struct names, so in typed mode those become fair
to demand -- and only there. A case declares the modes it is scored in, and the
same case can hold the baseline run to "a pointer came back" and the typed run
to "the pointer is spelled `EvalNode*`".
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# A C type as the emitter spells it, reduced to what a decompiler can be held to.
TYPE_WIDTHS = {
    "uint8_t": "8",
    "int8_t": "8",
    "uint16_t": "16",
    "int16_t": "16",
    "uint32_t": "32",
    "int32_t": "32",
    "uint64_t": "64",
    "int64_t": "64",
    "uint128_t": "128",
    "int128_t": "128",
    "bool": "8",
}


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def classify_type(text: str) -> str:
    """Width, or 'ptr'/'void', for one declared type."""
    # Comments first: the emitter puts the header's claim beside a type it could
    # not adopt (`uint64_t /* header says EvalVec3 */`), and the `*` in the
    # comment delimiters would otherwise read as a pointer.
    text = strip_comments(text).strip()
    if "*" in text:
        return "ptr"
    if text.startswith("void") or text == "void":
        return "void"
    for name, width in TYPE_WIDTHS.items():
        if re.search(rf"\b{name}\b", text):
            return width
    return "?"


def find_signature(text: str, name: str) -> tuple[str, str, int] | None:
    """The definition of `name`: its return type, its parameter list, and where
    its body starts. Definitions only -- a forward declaration ends in ';' and
    says nothing about what was recovered."""
    for match in re.finditer(
        rf"(?m)^((?:[\w\s\*]|/\*.*?\*/)*?)\b{re.escape(name)}\s*\(([^)]*)\)\s*\{{", text
    ):
        return match.group(1), match.group(2), match.end() - 1
    return None


def sections(text: str) -> dict[str, str]:
    """The combined output, split into one chunk per function.

    Everything a case is scored on has to come from that case's own chunk. A
    type definition is emitted in the preamble above the function that needed
    it, which is outside the body but still that function's output -- and
    searching the whole file instead would let one case pass on a definition
    another case's preamble happened to emit.
    """
    out: dict[str, str] = {}
    marks = list(re.finditer(r"(?m)^// =+ (\w+) @ 0x[0-9a-f]+ =+$", text))
    for index, mark in enumerate(marks):
        end = marks[index + 1].start() if index + 1 < len(marks) else len(text)
        out[mark.group(1)] = text[mark.end() : end]
    return out


def extract_body(text: str, brace_index: int) -> str | None:
    depth = 0
    for index in range(brace_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace_index : index + 1]
    return None


def split_params(params: str) -> list[str]:
    params = re.sub(r"/\*.*?\*/", "", params).strip()
    if not params or params == "void":
        return []
    return [p.strip() for p in params.split(",") if p.strip()]


def param_type(param: str) -> str:
    """The type of `uint32_t* a0`, i.e. everything but the trailing name."""
    param = re.sub(r"/\*.*?\*/", "", param).strip()
    return classify_type(re.sub(r"\b[A-Za-z_]\w*\s*$", "", param) or param)


def loop_conditions(body: str) -> list[str]:
    out = []
    for match in re.finditer(r"\b(?:while)\s*\(", body):
        start = match.end() - 1
        depth = 0
        for index in range(start, len(body)):
            if body[index] == "(":
                depth += 1
            elif body[index] == ")":
                depth -= 1
                if depth == 0:
                    out.append(body[start + 1 : index])
                    break
    return out


def measure(body: str) -> dict[str, int]:
    ifs = len(re.findall(r"\bif\s*\(", body))
    ternaries = len(re.findall(r"\?", body))
    return {
        "lines": body.count("\n"),
        "gotos": len(re.findall(r"\bgoto\b", body)),
        "labels": len(re.findall(r"^L_0x[0-9a-f]+:", body, re.M)),
        "switches": len(re.findall(r"\bswitch\s*\(", body)),
        "cases": len(re.findall(r"\bcase\s+", body)),
        "ifs": ifs,
        "ternaries": ternaries,
        # Either spelling is a recovered conditional; which one is a matter of
        # emission style, tracked separately so a change in style is visible.
        "conditionals": ifs + ternaries,
        "loops_while": len(re.findall(r"\bwhile\s*\(", body)),
        "loops_do": len(re.findall(r"\bdo\s*\{", body)),
        "loops_for": len(re.findall(r"\bfor\s*\(", body)),
        "returns": len(re.findall(r"\breturn\b", body)),
        "undef": body.count("/*undef*/"),
        "unnamed": body.count("/*unnamed-value"),
        "dead": body.count("/*dead-value*/"),
        "intrinsics": len(re.findall(r"__xdec_", body)),
        # Computed branches that resolution could not answer and --allow-unresolved
        # sealed as opaque. Each one is a hole in the CFG the reader has to fill
        # by hand, so it is the number that says how far short of the function
        # this output falls.
        "unresolved_branches": body.count("unresolved indirect branch at"),
        "unhandled": len(re.findall(r"/\*\w[\w.]*\?\*/0", body)),
        "temps": len(re.findall(r"^\s+uint\d+_t t\d+;", body, re.M)),
        # An svc that stayed an opaque intrinsic: the thing syscall recovery
        # exists to remove, counted so progress on it is a number.
        "svc_intrinsics": len(re.findall(r"__xdec_intrin_\w*\.?svc", body)),
        "syscall_named": len(re.findall(r"\bsys_[a-z_0-9]+\s*\(", body)),
        "syscall_raw": len(re.findall(r"__xdec_syscall\s*\(", body)),
        # Field access through a recovered struct type, as opposed to the
        # `*(int32_t*)(p + 8)` a decompiler writes when it has no type.
        "struct_arrow": body.count("->"),
    }


def check_signature(returns: str, params: str, expect: dict) -> list[str]:
    issues = []
    want_return = expect.get("return")
    got_return = classify_type(returns)
    if want_return is not None and got_return != want_return:
        issues.append(f"return type is {got_return}-bit, want {want_return}")

    want_params = expect.get("params")
    if want_params is None:
        return issues
    got = [param_type(p) for p in split_params(params)]
    if len(got) != len(want_params):
        issues.append(f"{len(got)} param(s), want {len(want_params)}")
    for index, want in enumerate(want_params):
        if index >= len(got):
            break
        if got[index] != want:
            issues.append(f"param {index} is {got[index]}, want {want}")
    return issues


def normalize(text: str) -> str:
    """A C type as written, with the spacing that carries no meaning removed, so
    `EvalVec3 *` and `EvalVec3*` compare equal."""
    return re.sub(r"\s+", "", re.sub(r"/\*.*?\*/", "", text))


def check_signature_exact(returns: str, params: str, expect: dict) -> list[str]:
    """Full spellings, not widths. Only meaningful in typed mode: the exact name
    of a type is knowledge that comes from a header, so demanding it of a run
    without one would be demanding a guess."""
    issues = []
    want_return = expect.get("return")
    if want_return is not None and normalize(returns) != normalize(want_return):
        issues.append(f"return spelled '{returns.strip()}', want '{want_return}'")

    want_params = expect.get("params")
    if want_params is None:
        return issues
    got = [
        re.sub(r"\b[A-Za-z_]\w*\s*$", "", p).strip() or p
        for p in split_params(params)
    ]
    if len(got) != len(want_params):
        issues.append(f"{len(got)} param(s), want {len(want_params)}")
    for index, want in enumerate(want_params):
        if index >= len(got):
            break
        if normalize(got[index]) != normalize(want):
            issues.append(f"param {index} spelled '{got[index]}', want '{want}'")
    return issues


def check_case(
    body: str | None,
    returns: str,
    params: str,
    expect: dict,
    signature: str = "",
    section: str = "",
) -> list[str]:
    if body is None:
        return ["function not found in output"]

    issues: list[str] = []
    m = measure(body)
    loops = m["loops_while"] + m["loops_do"] + m["loops_for"]

    if expect.get("max_gotos") is not None and m["gotos"] > expect["max_gotos"]:
        issues.append(f"gotos={m['gotos']} > max {expect['max_gotos']}")
    if expect.get("max_ternaries") is not None and m["ternaries"] > expect["max_ternaries"]:
        issues.append(f"ternaries={m['ternaries']} > max {expect['max_ternaries']}")
    if expect.get("min_ifs") is not None and m["ifs"] < expect["min_ifs"]:
        issues.append(f"ifs={m['ifs']} < min {expect['min_ifs']}")
    if expect.get("min_switches") is not None and m["switches"] < expect["min_switches"]:
        issues.append(f"switches={m['switches']} < min {expect['min_switches']}")
    if expect.get("min_loops") is not None and loops < expect["min_loops"]:
        issues.append(f"loops={loops} < min {expect['min_loops']}")
    if (
        expect.get("max_unresolved_branches") is not None
        and m["unresolved_branches"] > expect["max_unresolved_branches"]
    ):
        issues.append(
            f"unresolved_branches={m['unresolved_branches']} > max "
            f"{expect['max_unresolved_branches']}"
        )
    if expect.get("needs_return") and m["returns"] < 1:
        issues.append("no return statement")

    # The entry block must lead the body. A label ahead of it means the emitted
    # order is not a reading order, which is the defect and not a style choice.
    if expect.get("entry_first"):
        entry = re.search(r"^\s*// b0 @", body, re.M)
        label = re.search(r"^L_0x[0-9a-f]+:", body, re.M)
        if entry and label and label.start() < entry.start():
            issues.append("a label precedes the entry block")
        elif not entry:
            issues.append("entry block not marked in output")

    # A loop's condition has to be about something the loop changes. One written
    # purely in terms of parameters and constants either never terminates or --
    # far more likely -- is a loop-carried value that never got its merge, which
    # is a wrong answer rather than an ugly one.
    if expect.get("loop_condition_varies"):
        conditions = loop_conditions(body)
        if not conditions:
            issues.append("no while condition to inspect")
        else:
            fixed = [c for c in conditions if not re.search(r"\b(?:t\d+|_cse\d+)\b", c)]
            if fixed:
                issues.append(f"loop condition never changes: ({fixed[0][:40]})")

    if expect.get("max_svc_intrinsics") is not None:
        limit = expect["max_svc_intrinsics"]
        if m["svc_intrinsics"] > limit:
            issues.append(f"svc_intrinsics={m['svc_intrinsics']} > max {limit}")
    if expect.get("min_struct_arrow") is not None and m["struct_arrow"] < expect["min_struct_arrow"]:
        issues.append(f"struct_arrow={m['struct_arrow']} < min {expect['min_struct_arrow']}")

    if "signature" in expect:
        issues += check_signature(returns, params, expect["signature"])
    if "signature_exact" in expect:
        issues += check_signature_exact(returns, params, expect["signature_exact"])

    # Types are looked for across this function's whole output: a recovered
    # `EvalNode*` shows up as a parameter, its definition in the preamble above,
    # and its fields in the body, and a case should not have to say which.
    declared = section if section else signature + body
    for spelling in expect.get("type_spelling", []):
        if spelling not in declared:
            issues.append(f"missing type '{spelling}'")

    for pat in expect.get("patterns", []):
        if pat not in body:
            issues.append(f"missing pattern '{pat}'")
    # Regex, unlike `patterns` above, because what these say is "no output of
    # this shape", and the shape is the point: `__xdec_intrin.*svc` matches
    # however the intrinsic ends up spelled.
    for pat in expect.get("forbid_patterns", []):
        if re.search(pat, declared):
            issues.append(f"forbidden pattern '{pat}'")
    # Zero unless a case argues otherwise. On the NDK corpus any of these is a
    # defect, full stop; on a sample where resolution had to seal branches it
    # could not answer, blocks whose only predecessors were sealed away lose
    # their incoming dataflow, and the undef reads that follow are the cost of
    # the seal rather than a new bug. A case that owns that cost states it as a
    # number, so it stays visible and cannot grow unnoticed.
    for key in ("undef", "unnamed", "unhandled"):
        allowed = expect.get(f"max_{key}", 0)
        if m[key] > allowed:
            issues.append(f"{key}={m[key]} > max {allowed}")
    return issues


def expectations(case: dict, mode: str) -> dict:
    """The case's expectations for this mode: the shared ones, with the mode's
    own block layered on top. A case that says nothing mode-specific is checked
    the same way in both, which is what most cases want."""
    expect = dict(case.get("expect", {}))
    expect.update(case.get(f"expect_{mode}", {}))
    return expect


def main() -> int:
    argv = sys.argv[1:]
    mode = "baseline"
    baseline_path = None
    positional = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg == "--mode" and index + 1 < len(argv):
            mode = argv[index + 1]
            index += 2
        elif arg == "--baseline" and index + 1 < len(argv):
            baseline_path = Path(argv[index + 1])
            index += 2
        else:
            positional.append(arg)
            index += 1

    if len(positional) < 3:
        print(
            f"usage: {sys.argv[0]} <manifest.json> <combined.c> <report.json>"
            " [--mode baseline|typed] [--baseline <file.json>]",
            file=sys.stderr,
        )
        return 2

    manifest_path = Path(positional[0])
    c_path = Path(positional[1])
    report_path = Path(positional[2])
    if baseline_path is None and len(positional) > 3:
        baseline_path = Path(positional[3])

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    text = c_path.read_text(encoding="utf-8", errors="replace")
    by_section = sections(text)

    results = []
    for case in manifest["cases"]:
        name = case["name"]
        if not case.get("enabled", True):
            continue
        if mode not in case.get("modes", ["baseline", "typed"]):
            continue
        section = by_section.get(name, "")
        found = find_signature(section or text, name)
        if found is None:
            body, returns, params = None, "", ""
        else:
            returns, params, brace = found
            body = extract_body(section or text, brace)
        signature = f"{returns.strip()} {name}({params})" if found else ""
        issues = check_case(
            body, returns, params, expectations(case, mode), signature, section
        )
        results.append(
            {
                "name": name,
                "category": case.get("category", "?"),
                "source": case.get("source", ""),
                "emitted": signature,
                "ok": not issues,
                "issues": issues,
                "metrics": measure(body) if body else {},
            }
        )

    failed = sum(1 for r in results if not r["ok"])
    by_category: dict[str, list] = {}
    for r in results:
        by_category.setdefault(r["category"], []).append(r)

    report = {
        "manifest": str(manifest_path),
        "source_c": str(c_path),
        "mode": mode,
        "total": len(results),
        "passed": len(results) - failed,
        "failed": failed,
        "cases": results,
        "category_summary": {
            cat: {
                "total": len(items),
                "passed": sum(1 for i in items if i["ok"]),
            }
            for cat, items in sorted(by_category.items())
        },
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"eval [{mode}]: {report['passed']}/{report['total']} passed, {failed} failed")
    print()
    print(f"{'case':<26} {'category':<12} {'ok':<4} issues")
    print("-" * 100)
    for r in results:
        issue = "; ".join(r["issues"]) if r["issues"] else "-"
        print(f"{r['name']:<26} {r['category']:<12} {'yes' if r['ok'] else 'NO':<4} {issue}")

    print()
    print("By category:")
    for cat, s in report["category_summary"].items():
        print(f"  {cat:<14} {s['passed']}/{s['total']}")

    if baseline_path and baseline_path.exists():
        base = json.loads(baseline_path.read_text(encoding="utf-8"))
        base_mode = base.get("mode", "baseline")
        if base_mode != mode:
            print()
            print(f"warning: baseline was recorded in {base_mode} mode, comparing against {mode}")
        base_ok = {c["name"]: c["ok"] for c in base["cases"]}
        gained = [r["name"] for r in results if r["ok"] and not base_ok.get(r["name"], False)]
        lost = [r["name"] for r in results if not r["ok"] and base_ok.get(r["name"], False)]
        print()
        print(f"vs baseline ({base['passed']}/{base['total']}):")
        print(f"  fixed:     {', '.join(gained) if gained else '-'}")
        print(f"  regressed: {', '.join(lost) if lost else '-'}")
        if lost:
            return 2

    print()
    print(f"report: {report_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
