#!/usr/bin/env python3
"""Fuzz the decoder against capstone.

diff_operands.py walks a binary's instructions, which only ever exercises the
words a compiler happened to emit. This attacks the decode space itself: pure
random words, and real instructions with a few bits flipped, which live near
the encoding boundaries where a decision-tree bug would hide.

For every word the outcomes sort into:

    both reject          agreement; unallocated encodings
    both accept          mnemonics must match (aliases aside)
    xdec only            either the spec over-decodes or capstone is old
    capstone only        a hole in the spec — the interesting bucket

Usage: fuzz_decode.py <xdec> <spec> [--random N] [--flips N] [--seed N] [--binary PATH]
"""

import argparse
import collections
import random
import re
import struct
import subprocess
import sys

import capstone
import lief

# From diff_operands.py: mnemonics that differ only by which register file the
# alias prints, not by what the instruction does.
ALIASES = {
    # movz/movn print as mov with the complemented/shifted immediate folded in.
    frozenset({"mov", "orr", "add", "lsl", "sxtw", "uxtw", "movz", "movn"}),
    frozenset({"cmp", "subs"}),
    frozenset({"cmn", "adds"}),
    frozenset({"neg", "sub"}),
    frozenset({"negs", "subs"}),
    frozenset({"tst", "ands"}),
    frozenset({"b", "b.cond"}),
    frozenset({"mul", "madd"}),
    frozenset({"mneg", "msub"}),
    # hint #N covers every named hint; the encodings are identical.
    frozenset({"hint", "nop", "yield", "wfe", "wfi", "sev", "sevl", "esb", "dgh",
               "tsb", "psb", "csdb", "ssbb", "pssbb", "autibz", "autib1716",
               "paciaz", "pacibz", "autiaz", "paciz1716", "pacib1716", "autiz1716",
               "paciasp", "pacibsp", "autiasp", "autibsp",
               "xpaclri", "clrex", "isb", "dsb", "dmb"}),
    # Preferred bitfield aliases: bfc x, #lsb, #w is bfi/bfxil with xzr.
    frozenset({"bfc", "bfi", "bfxil"}),
}


def alias_group(mnemonic):
    base = mnemonic.split(".")[0]
    for group in ALIASES:
        if base in group:
            return group
    return frozenset({base})


def xdec_decode(xdec, words, spec):
    """One batch invocation of `xdec decode`."""
    proc = subprocess.run(
        [xdec, "decode"], input="\n".join(f"{w:#x}" for w in words),
        capture_output=True, text=True, check=True,
        env={**__import__("os").environ, "XDEC_SPEC": spec},
    )
    result = {}
    for line in proc.stdout.splitlines():
        match = re.match(r"^(0x[0-9a-f]+)\s+(.*)$", line)
        if match:
            text = match.group(2).strip()
            result[int(match.group(1), 16)] = None if text == "undecodable" else text
    return result


def sample_words(binary_path, rng):
    """Real instruction words from the binary's executable sections."""
    binary = lief.parse(binary_path)
    words = []
    for section in binary.sections:
        if not section.flags & lief.ELF.Section.FLAGS.EXECINSTR.value:
            continue
        data = bytes(section.content)
        words.extend(struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4]))
    rng.shuffle(words)
    return words


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("xdec")
    parser.add_argument("spec")
    parser.add_argument("--random", type=int, default=200000, dest="random_words")
    parser.add_argument("--flips", type=int, default=200000)
    parser.add_argument("--flip-bits", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--binary",
                        default=r"E:\workspace\af\com.intech.c66app__\libsdk_bc_lib.so")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    words = []
    if args.random_words:
        words.extend(rng.getrandbits(32) for _ in range(args.random_words))
    if args.flips:
        pool = sample_words(args.binary, rng)
        for index in range(args.flips):
            word = pool[index % len(pool)]
            flips = rng.sample(range(32), rng.randint(1, args.flip_bits))
            for bit in flips:
                word ^= 1 << bit
            words.append(word)

    buckets = collections.Counter()
    capstone_only = collections.Counter()
    xdec_only = collections.Counter()
    mnemonic_mismatch = collections.Counter()
    examples = {}

    mine = xdec_decode(args.xdec, words, args.spec)
    # capstone's streaming disassembler desyncs on bad words; decode one by one.
    for word in words:
        data = struct.pack("<I", word)
        cap = next(md.disasm(data, 0), None)
        got = mine.get(word)
        # A catch-all matched (the SIMD/ldst sinks): the spec accepted the word
        # but models nothing about it. Neither a hole nor a comparison.
        catchall = got is not None and got.split()[0] in ("simd", "ldst", ".word")
        if catchall:
            buckets["xdec-catchall"] += 1
            if cap is None:
                buckets["xdec-only"] += 1
            continue
        if cap is None and got is None:
            buckets["both-reject"] += 1
            continue
        if cap is None:
            buckets["xdec-only"] += 1
            key = got.split()[0]
            xdec_only[key] += 1
            examples.setdefault(("xdec-only", key), (word, None, got))
            continue
        if got is None:
            buckets["capstone-only"] += 1
            key = cap.mnemonic
            capstone_only[key] += 1
            examples.setdefault(("capstone-only", key), (word, f"{cap.mnemonic} {cap.op_str}", None))
            continue
        buckets["both-accept"] += 1
        cap_mnemonic = cap.mnemonic
        my_mnemonic = got.split()[0]
        if alias_group(cap_mnemonic) != alias_group(my_mnemonic):
            mnemonic_mismatch[(cap_mnemonic, my_mnemonic)] += 1
            examples.setdefault(("mismatch", (cap_mnemonic, my_mnemonic)),
                                (word, f"{cap_mnemonic} {cap.op_str}", got))

    total = len(words)
    print(f"{total} words")
    for bucket, count in sorted(buckets.items()):
        print(f"  {bucket:<14} {count:>8}  {100.0 * count / total:5.1f}%")
    if mnemonic_mismatch:
        print(f"\n  mnemonic mismatches: {sum(mnemonic_mismatch.values())}")
        for (cap_mnemonic, my_mnemonic), count in mnemonic_mismatch.most_common(15):
            word, cap_text, got = examples[("mismatch", (cap_mnemonic, my_mnemonic))]
            print(f"    {count:>6}  {word:#010x}  capstone: {cap_text}")
            print(f"    {'':>6}  {'':>10}  xdec:     {got}")
    if capstone_only:
        print(f"\n  decoded by capstone only: {sum(capstone_only.values())}")
        for mnemonic, count in capstone_only.most_common(15):
            word, cap_text, _ = examples[("capstone-only", mnemonic)]
            print(f"    {count:>6}  {word:#010x}  capstone: {cap_text}")
    if xdec_only:
        print(f"\n  decoded by xdec only: {sum(xdec_only.values())}")
        for mnemonic, count in xdec_only.most_common(10):
            word, _, got = examples[("xdec-only", mnemonic)]
            print(f"    {count:>6}  {word:#010x}  xdec: {got}")
    return 1 if mnemonic_mismatch else 0


if __name__ == "__main__":
    sys.exit(main())
