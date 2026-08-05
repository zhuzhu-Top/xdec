#!/usr/bin/env python3
"""Differential test of xdec's disassembly against capstone, operands included.

diff_capstone.py compares mnemonics, which answers "did we decode the right
instruction". This answers the harder question: did we decode the right
*operands*. A rule can name an instruction correctly and still have the wrong
register field or a shift off by one, and a wrong scale on a load offset is
invisible to a mnemonic comparison while being fatal to stack analysis.

Restrict it to the rules under test with a mnemonic filter, because the
unmodelled SIMD catch-alls print an opcode word and would drown the signal.

Usage: diff_operands.py <xdec> <binary> <spec> [mnemonic,...] [limit]
"""

import collections
import re
import subprocess
import sys

import capstone
import lief


def canonical_number(match):
    """`#-0x20` and `#-32` are the same displacement, printed by different tools.

    Capstone prints a negative post-index offset as its unsigned 64-bit value, so
    `0xfffffffffffffffc` and `-4` also have to compare equal.
    """
    text = match.group(0)
    negative = text.startswith("-")
    digits = text.lstrip("-")
    value = int(digits, 16) if digits.startswith("0x") else int(digits)
    if negative:
        value = -value
    if value >= 1 << 63:
        value -= 1 << 64
    return str(value)


def normalise(text):
    """Spelling differences that are not disagreements about what was decoded."""
    # capstone writes a branch target as `#0x1000` and xdec as `0x1000`. Dropping
    # every `#` costs nothing, since both sides lose the ones before immediates.
    text = text.lower().replace(" ", "").replace("#", "")
    text = re.sub(r"-?(?:0x[0-9a-f]+|\d+)", canonical_number, text)
    # capstone prints an omitted zero offset as `[x0]`, and a present one as
    # `[x0, #0]`; both mean the same address. Done after the numbers are
    # canonical so that `#0x0` is caught along with `#0`.
    return text.replace(",0]", "]")


def xdec_disasm(xdec, binary, address, count, spec):
    out = subprocess.run(
        [xdec, "disasm", binary, hex(address), str(count)],
        capture_output=True, text=True, check=True,
        env={**__import__("os").environ, "XDEC_SPEC": spec},
    ).stdout
    result = {}
    for line in out.splitlines():
        match = re.match(r"^(0x[0-9a-f]+)\s+([0-9a-f]{8})\s+(.*)$", line)
        if match:
            # `disasm` follows the text with a control-flow annotation in its own
            # column. Two spaces separate them; one never appears inside operands.
            text = re.split(r"\s{2,}", match.group(3).strip())[0]
            result[int(match.group(1), 16)] = text
    return result


def main(xdec, path, spec, mnemonics="", limit=200000):
    wanted = {m for m in mnemonics.split(",") if m and m != "-"}
    binary = lief.parse(path)
    text = next(s for s in binary.sections if s.name == ".text")
    count = min(int(limit), text.size // 4)

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    data = bytes(text.content)[: count * 4]
    truth = {i.address: (i.mnemonic, f"{i.mnemonic} {i.op_str}".strip())
             for i in md.disasm(data, text.virtual_address)}

    mine = {}
    for start in range(0, count, 8192):
        chunk = min(8192, count - start)
        mine.update(xdec_disasm(xdec, path, text.virtual_address + start * 4, chunk, spec))

    checked = 0
    agree = 0
    unmodelled = 0
    disagree = collections.Counter()
    examples = {}
    for address, (mnemonic, expected) in truth.items():
        if wanted and mnemonic not in wanted:
            continue
        got = mine.get(address)
        if got is None:
            continue
        # The catch-alls print an opcode word rather than operands. They are
        # counted by `xdec coverage`, and comparing their text to capstone's
        # would say nothing about the rules that do model their operands.
        if got.split()[0] in ("simd", "ldst", ".word"):
            unmodelled += 1
            continue
        checked += 1
        if normalise(expected) == normalise(got):
            agree += 1
        else:
            key = (expected.split()[0], got.split()[0])
            disagree[key] += 1
            examples.setdefault(key, (address, expected, got))

    print(f"{path}")
    print(f"  {checked} operand comparisons over {sorted(wanted) or 'all mnemonics'}"
          f", {unmodelled} left to a catch-all")
    if checked:
        print(f"  agree     {agree:>7}  {100.0*agree/checked:6.2f}%")
        print(f"  disagree  {sum(disagree.values()):>7}  "
              f"{100.0*sum(disagree.values())/checked:6.2f}%")
    for key, n in disagree.most_common(20):
        address, expected, got = examples[key]
        print(f"    {n:>6}  {address:#x}  capstone: {expected}")
        print(f"            {'':>{len(hex(address))}}  xdec:     {got}")
    return 1 if disagree else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
