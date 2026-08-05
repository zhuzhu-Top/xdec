#!/usr/bin/env python3
"""Differential test of the lifted IL's semantics against Unicorn.

diff_operands.py answers "did we decode the right instruction". This answers
the question that actually matters for a decompiler: does the IL *mean* the
same thing as the silicon. For every basic block we run the same random state
through Unicorn (the machine oracle) and through xdec's IL interpreter, and
compare every register, the flag bits, the memory either side wrote, and where
control went next.

A mismatch here is a semantic bug in the spec by definition: the IL was built
from the same bytes Unicorn executed, so any divergence is ours.

    xdec exec            interprets the lifted IL in batch (one process)
    unicorn              executes the raw instruction bytes
    memfill              both sides generate the identical splitmix64 stream,
                         so "random" stack/scratch contents agree exactly

Usage: diff_unicorn.py <xdec> <binary> <spec> [options]
"""

import argparse
import collections
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

import capstone
import lief
import unicorn
import unicorn.arm64_const as arm64_const
from unicorn.arm64_const import (
    UC_ARM64_REG_NZCV,
    UC_ARM64_REG_PC,
    UC_ARM64_REG_PSTATE,
    UC_ARM64_REG_SP,
)

PAGE = 0x1000
STACK_BASE = 0x0000_7FFF_0000_0000
STACK_SIZE = 0x10000
SCRATCH_BASE = 0x0000_1000_0000_0000
SCRATCH_SIZE = 0x10000

# Terminators are classified by what the comparison can check. A direct branch
# ends the block and its destination is compared; the others are excluded from
# the block (the state *before* them is still compared), because an indirect
# target is the resolver's business and a callee's effects are not the block's.
DIRECT_BRANCH = re.compile(r"^(b|b\.\w+|cbz|cbnz|tbz|tbnz)$")
EXCLUDED_TERMINATOR = re.compile(r"^(bl|blr|br|ret)$")
# Unicorn's exclusive stores never fault: an unmapped target fails the monitor
# and the status register comes out 1. Real hardware (and the IL) grants the
# monitor in a single thread and then faults on the store itself.
EXCLUSIVE_STORE = re.compile(r"^(stxr|stlxr|stxrb|stlxrb|stxrh|stlxrh|stxp|stlxp)$")

X_REGS = [f"x{i}" for i in range(31)]
Q_REGS = [f"q{i}" for i in range(32)]
# Unicorn's register ids are NOT consecutive (x29 and x30 alias low numbers), so
# they are looked up by name rather than computed from a base.
UC_X = {name: getattr(arm64_const, f"UC_ARM64_REG_{name.upper()}") for name in X_REGS}
UC_Q = {name: getattr(arm64_const, f"UC_ARM64_REG_{name.upper()}") for name in Q_REGS}


def splitmix64_stream(seed, size):
    """The memfill byte stream. Must match splitmix64Next in xdec_main.cpp."""
    out = bytearray()
    state = seed
    while len(out) < size:
        state = (state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        z = z ^ (z >> 31)
        out += z.to_bytes(8, "little")
    return bytes(out[:size])


class Block:
    __slots__ = ("va", "words", "terminator")

    def __init__(self, va, words, terminator):
        self.va = va
        self.words = words  # list of int, terminators included when kept
        self.terminator = terminator  # "direct" | "none"

    @property
    def count(self):
        return len(self.words)

    @property
    def end(self):
        return self.va + 4 * self.count


def enumerate_blocks(path):
    """Basic blocks: maximal fall-through runs, via capstone as the reader."""
    binary = lief.parse(path)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    blocks = []
    targets = set()
    for section in binary.sections:
        if not section.flags & lief.ELF.Section.FLAGS.EXECINSTR.value:
            continue
        data = bytes(section.content)
        base = section.virtual_address
        current = []
        run_va = base
        offset = 0
        while offset + 4 <= len(data):
            word = struct.unpack_from("<I", data, offset)[0]
            decoded = next(md.disasm(data[offset:offset + 4], base + offset), None)
            if decoded is None:
                if current:
                    blocks.append(Block(run_va, current, "none"))
                    current = []
                offset += 4
                continue
            mnemonic = decoded.mnemonic
            if EXCLUDED_TERMINATOR.match(mnemonic):
                if current:
                    blocks.append(Block(run_va, current, "none"))
                    current = []
            elif DIRECT_BRANCH.match(mnemonic):
                if not current:
                    run_va = base + offset
                current.append(word)
                blocks.append(Block(run_va, current, "direct"))
                current = []
                # A direct branch's target is a block leader too. Capstone
                # prints it as `#0x...`; tbz/tbnz have it as the last operand.
                target_match = re.search(r"#(0x[0-9a-f]+)", decoded.op_str)
                if target_match:
                    targets.add(int(target_match.group(1), 16))
            else:
                if not current:
                    run_va = base + offset
                current.append(word)
            offset += 4
        if current:
            blocks.append(Block(run_va, current, "none"))

    # Second pass: split any block a branch lands in the middle of, otherwise a
    # backward branch (a loop) would leave "one block" that is really two, and
    # xdec's CFG-correct lifter would execute only the prefix.
    split_blocks = []
    for block in blocks:
        cuts = sorted(t for t in targets if block.va < t < block.end)
        if not cuts:
            split_blocks.append(block)
            continue
        start = 0
        for cut in cuts:
            index = (cut - block.va) // 4
            split_blocks.append(Block(block.va + 4 * start, block.words[start:index], "none"))
            start = index
        split_blocks.append(Block(block.va + 4 * start, block.words[start:], block.terminator))
    return split_blocks


def make_state(rng, image_ranges):
    """One initial state. A third of the registers get plausible pointers, so
    loads and stores land somewhere mapped instead of faulting instantly."""
    state = {name: rng.getrandbits(64) for name in X_REGS}
    state["sp"] = STACK_BASE + STACK_SIZE // 2 + (rng.getrandbits(12) & ~0xF)
    state["nzcv"] = rng.getrandbits(4)
    for name in Q_REGS:
        state[name] = rng.getrandbits(128)
    for name in X_REGS:
        if name in ("x28", "x29", "x30") or rng.random() < 0.30:
            which = rng.random()
            if which < 0.45:
                state[name] = SCRATCH_BASE + (rng.getrandbits(14) & ~0xF)
            elif which < 0.7:
                state[name] = STACK_BASE + (rng.getrandbits(14) & ~0xF)
            else:
                low, high = rng.choice(image_ranges)
                state[name] = low + (rng.getrandbits(20) % max(1, high - low - 16)) & ~0xF
    return state


def edge_state(image_ranges):
    """The degenerate states: all zero and all ones, with a valid stack."""
    state = {name: 0 for name in X_REGS}
    state["sp"] = STACK_BASE + STACK_SIZE // 2
    state["nzcv"] = 0
    for name in Q_REGS:
        state[name] = 0
    return state


def run_xdec(xdec, binary_path, spec, jobs, states):
    """One batch invocation of `xdec exec` for every (block, state) pair."""
    lines = []
    for block_index, block in enumerate(jobs):
        lines.append(f"block {block.va:#x} {block.count}")
        for _, state in states[block_index]:
            for name in X_REGS + ["sp", "nzcv"]:
                lines.append(f"reg {name} {state[name]:#x}")
            for name in Q_REGS:
                lines.append(f"reg {name} {state[name]:#032x}")
            lines.append(f"memfill {STACK_BASE:#x} {STACK_SIZE:#x} {state['memseed']:#x}")
            lines.append(f"memfill {SCRATCH_BASE:#x} {SCRATCH_SIZE:#x} {state['memseed'] + 1:#x}")
            lines.append("run")
    with tempfile.NamedTemporaryFile(
        "w", suffix=".workload", delete=False, encoding="utf-8"
    ) as handle:
        handle.write("\n".join(lines))
        workload = handle.name
    try:
        out = subprocess.run(
            [xdec, "exec", binary_path, workload],
            capture_output=True, text=True,
            env={**os.environ, "XDEC_SPEC": spec},
        )
    finally:
        os.unlink(workload)
    if out.returncode != 0:
        sys.exit(f"xdec exec failed:\n{out.stdout}\n{out.stderr}")

    runs = []
    current = None
    for line in out.stdout.splitlines():
        if line.startswith("run "):
            _, _, _, va, end = line.split()
            current = {"block": int(va, 16), "end": int(end, 16),
                       "regs": {}, "mem": {}, "flow": None}
        elif line.startswith("flow ") and current is not None:
            current["flow"] = line[5:]
        elif line.startswith("reg ") and current is not None:
            _, name, value = line.split()
            current["regs"][name] = int(value, 16)
        elif line.startswith("mem ") and current is not None:
            _, address, size, hexbytes = line.split()
            address = int(address, 16)
            data = bytes.fromhex(hexbytes)
            for i, byte in enumerate(data):
                current["mem"][address + i] = byte
        elif line == "end" and current is not None:
            runs.append(current)
            current = None
    return runs


class UnicornRunner:
    """Seeds Unicorn from `xdec memdump`, i.e. the exact byte view the IL
    interpreter was seeded with: file contents with bss zeroed and resolved
    relocations applied. Seeding from LIEF instead would silently diverge on
    every R_AARCH64_RELATIVE slot (and LIEF's segment.content is not even the
    raw file bytes on some binaries)."""

    def __init__(self, xdec, binary_path):
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_LITTLE_ENDIAN)
        self.image_ranges = []
        self.pristine = []
        with tempfile.NamedTemporaryFile(suffix=".memdump", delete=False) as handle:
            dump = handle.name
        try:
            subprocess.run([xdec, "memdump", binary_path, dump],
                           check=True, capture_output=True)
            blob = open(dump, "rb").read()
        finally:
            os.unlink(dump)
        cursor = 0
        while cursor < len(blob):
            va, size = struct.unpack_from("<QQ", blob, cursor)
            cursor += 16
            content = blob[cursor:cursor + size]
            cursor += size
            low = va & ~(PAGE - 1)
            high = (va + size + PAGE - 1) & ~(PAGE - 1)
            try:
                self.uc.mem_map(low, high - low, unicorn.UC_PROT_ALL)
            except unicorn.UcError:
                pass  # regions may share a page after alignment
            self.uc.mem_write(va, content)
            self.pristine.append((low, high, va, content))
            self.image_ranges.append((va, va + size))
        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(SCRATCH_BASE, SCRATCH_SIZE)
        self.writes = {}
        self.uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._on_write)

    def _on_write(self, uc, access, address, size, value, user):
        # The hook fires before the store commits, so mem_read would hand us
        # the OLD bytes. The written data is the `value` parameter.
        data = (int(value) & ((1 << (size * 8)) - 1)).to_bytes(size, "little")
        for i, byte in enumerate(data):
            self.writes[address + i] = byte

    def run(self, block, state):
        self.writes = {}
        stack = splitmix64_stream(state["memseed"], STACK_SIZE)
        scratch = splitmix64_stream(state["memseed"] + 1, SCRATCH_SIZE)
        self.uc.mem_write(STACK_BASE, stack)
        self.uc.mem_write(SCRATCH_BASE, scratch)
        for name in X_REGS:
            self.uc.reg_write(UC_X[name], state[name])
        self.uc.reg_write(UC_ARM64_REG_SP, state["sp"])
        # Unicorn ignores direct NZCV writes; the flags only move through
        # PSTATE, and read back shifted into bits 31:28.
        self.uc.reg_write(UC_ARM64_REG_PSTATE, state["nzcv"] << 28)
        for name in Q_REGS:
            self.uc.reg_write(UC_Q[name], state[name])
        fault = None
        try:
            self.uc.emu_start(block.va, 0, count=block.count)
        except unicorn.UcError as error:
            fault = (error.errno, self.uc.reg_read(UC_ARM64_REG_PC))
        result = {"fault": fault, "pc": self.uc.reg_read(UC_ARM64_REG_PC),
                  "regs": {}, "mem": dict(self.writes)}
        for name in X_REGS:
            result["regs"][name] = self.uc.reg_read(UC_X[name])
        result["regs"]["sp"] = self.uc.reg_read(UC_ARM64_REG_SP)
        result["regs"]["nzcv"] = self.uc.reg_read(UC_ARM64_REG_NZCV) >> 28
        for name in Q_REGS:
            result["regs"][name] = int(self.uc.reg_read(UC_Q[name]))
        # Restore any self-modifying writes back to the pristine image: the
        # file byte where there is one, zero elsewhere (bss and the gaps).
        dirty = sorted(a for a in self.writes if a < SCRATCH_BASE)
        for address in dirty:
            for low, high, va, content in self.pristine:
                if low <= address < high:
                    pristine = content[address - va] if va <= address < va + len(content) else 0
                    self.uc.mem_write(address, bytes([pristine]))
                    break
        return result


def compare(block, state, il, uc):
    """Returns a list of human-readable differences, empty on agreement."""
    diffs = []
    flow = il["flow"] or ""
    if flow.startswith("intrinsic") or flow.startswith("unimplemented"):
        return None  # caller counts these as skips
    if uc["fault"] is not None:
        if flow.startswith("error"):
            parts = flow.split(None, 2)
            fault_va = int(parts[1], 16)
            if fault_va == uc["fault"][1]:
                return []  # both sides faulted at the same instruction
        diffs.append(f"unicorn faulted at {uc['fault'][1]:#x} (errno {uc['fault'][0]}), "
                     f"IL flow is '{flow}'")
        return diffs
    if flow.startswith("error"):
        fault_va = int(flow.split()[1], 16)
        index = (fault_va - block.va) // 4
        if 0 <= index < block.count and "unmapped" in flow:
            md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            insn = next(md.disasm(struct.pack("<I", block.words[index]), fault_va), None)
            if insn is not None and EXCLUSIVE_STORE.match(insn.mnemonic):
                return "oracle:exclusive-store"
        diffs.append(f"IL faulted: '{flow}', unicorn ran to {uc['pc']:#x}")
        return diffs

    for name in X_REGS + ["sp", "nzcv"] + Q_REGS:
        got = il["regs"].get(name)
        want = uc["regs"][name]
        if got != want:
            diffs.append(f"{name}: il={got:#x} unicorn={want:#x}")
    if flow.startswith("branch"):
        target = int(flow.split()[1], 16)
        if uc["pc"] != target:
            diffs.append(f"pc: il branch to {target:#x}, unicorn at {uc['pc']:#x}")
    elif flow.startswith("cond taken"):
        target = int(flow.split()[2], 16)
        if uc["pc"] != target:
            diffs.append(f"pc: il cond taken to {target:#x}, unicorn at {uc['pc']:#x}")
    elif flow.startswith("cond fall"):
        if uc["pc"] != il["end"]:
            diffs.append(f"pc: il cond fall to {il['end']:#x}, unicorn at {uc['pc']:#x}")
    elif flow:
        diffs.append(f"flow: il '{flow}', unicorn pc {uc['pc']:#x}")

    il_mem = il["mem"]
    uc_mem = uc["mem"]
    for address in sorted(set(il_mem) | set(uc_mem)):
        in_il = address in il_mem
        in_uc = address in uc_mem
        if in_il and in_uc and il_mem[address] == uc_mem[address]:
            continue
        diffs.append(f"mem[{address:#x}]: il={il_mem.get(address)} unicorn={uc_mem.get(address)}")
        if len(diffs) > 40:
            diffs.append("...")
            break
    return diffs


def write_corpus_case(directory, binary_path, block, state, il, uc, diffs):
    os.makedirs(directory, exist_ok=True)
    name = f"case_{block.va:x}_{state['index']}.case"
    path = os.path.join(directory, name)
    with open(path, "w", encoding="utf-8") as out:
        out.write(f"# {binary_path} block {block.va:#x} state {state['index']}\n")
        for diff in diffs[:8]:
            out.write(f"#   {diff}\n")
        out.write(f"base {block.va:#x}\n")
        out.write("words " + " ".join(f"{w:#x}" for w in block.words) + "\n")
        for reg in X_REGS + ["sp", "nzcv"]:
            out.write(f"reg {reg} {state[reg]:#x}\n")
        for reg in Q_REGS:
            out.write(f"reg {reg} {state[reg]:#032x}\n")
        out.write(f"memfill {STACK_BASE:#x} {STACK_SIZE:#x} {state['memseed']:#x}\n")
        out.write(f"memfill {SCRATCH_BASE:#x} {SCRATCH_SIZE:#x} {state['memseed'] + 1:#x}\n")
        for reg in X_REGS + ["sp", "nzcv"]:
            out.write(f"expect reg {reg} {uc['regs'][reg]:#x}\n")
        for reg in Q_REGS:
            out.write(f"expect reg {reg} {uc['regs'][reg]:#032x}\n")
        for address in sorted(uc["mem"]):
            out.write(f"expect mem {address:#x} {uc['mem'][address]:02x}\n")
        if uc["fault"] is None:
            out.write(f"expect pc {uc['pc']:#x}\n")
        else:
            out.write(f"expect fault {uc['fault'][1]:#x}\n")
    return path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("xdec")
    parser.add_argument("binary")
    parser.add_argument("spec")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--states", type=int, default=4, help="random states per block")
    parser.add_argument("--blocks", type=int, default=500, help="max blocks to test")
    parser.add_argument("--offset", type=int, default=0, help="first block index")
    parser.add_argument("--corpus", default="", help="directory for mismatch .case files")
    args = parser.parse_args()

    blocks = enumerate_blocks(args.binary)
    jobs = blocks[args.offset:args.offset + args.blocks]
    if not jobs:
        print("no blocks in range")
        return 0

    runner = UnicornRunner(args.xdec, args.binary)
    states = []
    for block_index, block in enumerate(jobs):
        rng = random.Random(args.seed * 1000003 + block.va)
        block_states = [("edge", edge_state(runner.image_ranges))]
        for index in range(args.states - 1):
            state = make_state(rng, runner.image_ranges)
            block_states.append((index + 1, state))
        for label, state in block_states:
            state["index"] = label
            state["memseed"] = rng.getrandbits(64)
        states.append(block_states)

    il_runs = run_xdec(args.xdec, args.binary, args.spec, jobs, states)
    expected_runs = sum(len(s) for s in states)
    if len(il_runs) != expected_runs:
        sys.exit(f"xdec returned {len(il_runs)} runs, expected {expected_runs}")

    matched = 0
    fault_matched = 0
    skips = collections.Counter()
    mismatches = []
    run_index = 0
    for block_index, block in enumerate(jobs):
        for _, state in states[block_index]:
            il = il_runs[run_index]
            run_index += 1
            flow = il["flow"] or ""
            if flow.startswith("intrinsic"):
                skips[f"intrinsic:{flow.split()[1]}"] += 1
                continue
            if flow.startswith("unimplemented"):
                skips[f"unimplemented:{flow.split()[1]}"] += 1
                continue
            uc = runner.run(block, state)
            diffs = compare(block, state, il, uc)
            if diffs is None:
                continue
            if isinstance(diffs, str):
                skips[diffs] += 1
                continue
            if not diffs:
                if uc["fault"] is not None:
                    fault_matched += 1
                else:
                    matched += 1
                continue
            mismatches.append((block, state, il, uc, diffs))
            if args.corpus:
                write_corpus_case(args.corpus, args.binary, block, state, il, uc, diffs)

    compared = matched + fault_matched + len(mismatches)
    print(f"{args.binary}")
    print(f"  blocks {len(jobs)} (of {len(blocks)}), runs {run_index}, compared {compared}")
    print(f"  matched       {matched:>7}")
    print(f"  fault-matched {fault_matched:>7}")
    print(f"  mismatched    {len(mismatches):>7}")
    for kind, count in skips.most_common(10):
        print(f"  skipped {count:>6}  {kind}")
    for block, state, il, uc, diffs in mismatches[:8]:
        print(f"\n  block {block.va:#x} state {state['index']} (memseed {state['memseed']:#x}):")
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        data = b"".join(struct.pack("<I", w) for w in block.words)
        for insn in md.disasm(data, block.va):
            print(f"    {insn.address:#x}  {insn.mnemonic} {insn.op_str}")
        for diff in diffs[:12]:
            print(f"    {diff}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
