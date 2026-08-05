"""Differential test of the xdec decoder against Capstone.

Coverage says the spec claims every word. It says nothing about whether the
claim is right, and a decoder that confidently reports the wrong mnemonic is
worse than one that reports nothing. Capstone is an independent implementation
of the same manual, so where the two disagree at least one is wrong.

Compares mnemonics only. Operand syntax differs legitimately between
disassemblers, and normalising it would mean encoding this decoder's opinions
into the oracle.
"""

import collections
import re
import subprocess
import sys

import capstone
import lief

# Places where the two disagree by convention rather than by fact. Capstone
# prefers some aliases that this spec does not write, and the reverse.
EQUIVALENT = [
    {"mov", "movz", "movn", "orr", "add"},
    {"cmp", "subs"},
    {"cmn", "adds"},
    {"tst", "ands"},
    {"neg", "sub"},
    {"negs", "subs"},
    {"mvn", "orn"},
    {"mul", "madd"},
    {"mneg", "msub"},
    {"lsl", "lslv", "ubfm", "ubfiz"},
    {"lsr", "lsrv", "ubfm"},
    {"asr", "asrv", "sbfm"},
    {"ror", "rorv", "extr"},
    {"sxtb", "sxth", "sxtw", "sbfm", "sbfx", "sbfiz"},
    {"uxtb", "uxth", "ubfm", "ubfx", "ubfiz"},
    {"bfi", "bfxil", "bfm", "bfc"},
    {"cset", "csetm", "cinc", "cinv", "cneg", "csinc", "csinv", "csneg"},
    {"nop", "hint", "yield", "wfe", "wfi", "sev", "sevl", "bti", "paciasp",
     "autiasp", "pacibsp", "autibsp", "xpaclri", "esb", "csdb"},
    {"smull", "smaddl"},
    {"umull", "umaddl"},
    {"smnegl", "smsubl"},
    {"rev", "rev64", "rev32", "rev16"},
    {"ldrsw", "ldursw"},
    # hs and cs name the same condition, as do lo and cc.
    {"b.hs", "b.cs"},
    {"b.lo", "b.cc"},
]


def build_closure():
    """Merges groups that share a member.

    Without this the table quietly lies: `sbfm` appears in both the `asr` group
    and the `sxtw` group, so a first-match lookup returns whichever was written
    first and every `sxtw` is reported as a disagreement.
    """
    parent = {}

    def find(name):
        parent.setdefault(name, name)
        while parent[name] != name:
            parent[name] = parent[parent[name]]
            name = parent[name]
        return name

    for group in EQUIVALENT:
        members = list(group)
        for other in members[1:]:
            parent[find(other)] = find(members[0])
    return {name: find(name) for name in parent}


CLOSURE = build_closure()


def alias_group(mnemonic):
    return CLOSURE.get(mnemonic, mnemonic)


def xdec_disasm(xdec, binary, address, count, spec):
    out = subprocess.run(
        [xdec, "disasm", binary, hex(address), str(count)],
        capture_output=True, text=True, check=True,
        env={**__import__("os").environ, "XDEC_SPEC": spec} if spec else None,
    ).stdout
    result = {}
    for line in out.splitlines():
        match = re.match(r"^(0x[0-9a-f]+)\s+([0-9a-f]{8})\s+(\S+)", line)
        if match:
            result[int(match.group(1), 16)] = match.group(3)
    return result


def main(xdec, path, spec=None, limit=40000):
    binary = lief.parse(path)
    text = next(s for s in binary.sections if s.name == ".text")
    count = min(int(limit), text.size // 4)

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    data = bytes(text.content)[: count * 4]
    truth = {i.address: i.mnemonic for i in md.disasm(data, text.virtual_address)}

    # Chunked so that one subprocess does not have to buffer the whole section.
    mine = {}
    for start in range(0, count, 8192):
        chunk = min(8192, count - start)
        mine.update(xdec_disasm(xdec, path, text.virtual_address + start * 4, chunk, spec))

    agree = 0
    undecoded = 0
    disagree = collections.Counter()
    examples = {}
    for address, expected in truth.items():
        got = mine.get(address)
        if got is None:
            continue
        if got == ".word":
            undecoded += 1
        elif alias_group(expected) == alias_group(got) or expected == got:
            agree += 1
        else:
            disagree[(expected, got)] += 1
            examples.setdefault((expected, got), address)

    total = len(truth)
    print(f"{path}")
    print(f"  {total} words compared")
    print(f"  agree      {agree:>7}  {100.0*agree/total:6.2f}%")
    print(f"  undecoded  {undecoded:>7}  {100.0*undecoded/total:6.2f}%")
    print(f"  disagree   {sum(disagree.values()):>7}  "
          f"{100.0*sum(disagree.values())/total:6.2f}%")
    if disagree:
        print("\n  top disagreements (capstone -> xdec):")
        for (expected, got), n in disagree.most_common(25):
            print(f"    {expected:<12} -> {got:<24} {n:>7}  e.g. "
                  f"{examples[(expected, got)]:#x}")
    return 1 if disagree else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
