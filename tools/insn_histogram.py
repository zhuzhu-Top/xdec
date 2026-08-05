"""Mnemonic frequency across the sample binaries' executable sections.

Written to decide what the ARM64 spec must cover, in what order. Guessing at the
list produces a spec that handles `fmla` before it handles `stp`, which is the
wrong way round by a factor of a thousand.
"""

import collections
import sys

import capstone
import lief


def histogram(path):
    binary = lief.parse(path)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    counts = collections.Counter()
    total = 0
    for section in binary.sections:
        if not (section.flags & 0x4):  # SHF_EXECINSTR
            continue
        data = bytes(section.content)
        decoded = 0
        for insn in md.disasm(data, section.virtual_address):
            counts[insn.mnemonic] += 1
            decoded += 1
        total += len(data) // 4
    return counts, total


def main(paths):
    combined = collections.Counter()
    grand = 0
    for path in paths:
        counts, total = histogram(path)
        print(f"{path}: {sum(counts.values())} decoded of {total} words")
        combined.update(counts)
        grand += total

    print(f"\n{len(combined)} distinct mnemonics over {grand} words\n")
    running = 0
    everything = sum(combined.values())
    for rank, (mnemonic, count) in enumerate(combined.most_common(), 1):
        running += count
        share = 100.0 * running / everything
        print(f"{rank:3}  {mnemonic:<12} {count:>8}  {100.0*count/everything:5.2f}%  cum {share:6.2f}%")
        if share > 99.5:
            remaining = len(combined) - rank
            print(f"\n... {remaining} more mnemonics make up the last {100-share:.2f}%")
            break


if __name__ == "__main__":
    main(sys.argv[1:])
