#!/usr/bin/env python3
"""Score L1 sample decompiles against manifest.json's structural expectations.

There is no ground truth C for these binaries, so this is deliberately a thin
wrapper around eval/score.py's measurement and shape-checking logic (gotos,
switches, ternaries, forbid_patterns, ...). What it does not use from there is
anything that needs a known source: signature/signature_exact/type_spelling
checks still work if a case supplies them (e.g. after `--types`), but no case
here is expected to.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "eval"))
from score import check_case, extract_body, measure, sections  # noqa: E402

# A case names a function only by address, not by the symbol name eval/'s
# find_signature expects -- the binary may have no symbol there at all. Each
# case still decompiles exactly one function, so apart from the emitter's own
# prelude the section holds exactly one definition (body-having, as opposed to
# the `sub_1234();` forward declarations xdec emits for callees it did not
# decompile); whichever one that is *is* the target, under whatever name xdec
# gave it.
_DEFINITION = re.compile(r"(?m)^((?:[\w\s\*]|/\*.*?\*/)*?)\b(\w+)\s*\(([^)]*)\)\s*\{")


def find_target(text: str) -> tuple[str, str, str, int] | None:
    for match in _DEFINITION.finditer(text):
        # The prelude the emitter writes ahead of the function it was asked for
        # -- __xdec_rotr32 and friends, present only when the body needs them.
        # Scoring one of those would silently measure three lines of helper and
        # call the case clean.
        if match.group(2).startswith("__xdec_"):
            continue
        return match.group(2), match.group(1), match.group(3), match.end() - 1
    return None


def main() -> int:
    argv = sys.argv[1:]
    baseline_path = None
    positional = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg == "--baseline" and index + 1 < len(argv):
            baseline_path = Path(argv[index + 1])
            index += 2
        else:
            positional.append(arg)
            index += 1

    if len(positional) < 3:
        print(
            f"usage: {sys.argv[0]} <manifest.json> <combined.c> <report.json> [--baseline <file.json>]",
            file=sys.stderr,
        )
        return 2

    manifest_path = Path(positional[0])
    c_path = Path(positional[1])
    report_path = Path(positional[2])

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    text = c_path.read_text(encoding="utf-8", errors="replace")
    by_section = sections(text)

    results = []
    for case in manifest["cases"]:
        name = case["name"]
        if not case.get("enabled", True):
            continue
        section = by_section.get(name)
        if section is None:
            # Not decompiled this run (missing binary, or the decompile
            # itself failed) -- run.ps1 already reported why.
            results.append(
                {
                    "name": name,
                    "category": case.get("category", "?"),
                    "emitted": "",
                    "ok": False,
                    "issues": ["not decompiled this run"],
                    "metrics": {},
                }
            )
            continue
        found = find_target(section)
        if found is None:
            body, returns, params, symbol = None, "", "", ""
        else:
            symbol, returns, params, brace = found
            body = extract_body(section, brace)
        signature = f"{returns.strip()} {symbol}({params})" if found else ""
        issues = check_case(body, returns, params, case.get("expect", {}), signature, section)
        results.append(
            {
                "name": name,
                "category": case.get("category", "?"),
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
        "total": len(results),
        "passed": len(results) - failed,
        "failed": failed,
        "cases": results,
        "category_summary": {
            cat: {"total": len(items), "passed": sum(1 for i in items if i["ok"])}
            for cat, items in sorted(by_category.items())
        },
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"samples: {report['passed']}/{report['total']} passed, {failed} failed")
    print()
    print(f"{'case':<26} {'category':<12} {'ok':<4} issues")
    print("-" * 100)
    for r in results:
        issue = "; ".join(r["issues"]) if r["issues"] else "-"
        print(f"{r['name']:<26} {r['category']:<12} {'yes' if r['ok'] else 'NO':<4} {issue}")

    if baseline_path and baseline_path.exists():
        base = json.loads(baseline_path.read_text(encoding="utf-8"))
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
