# 04 — Validation

A decompiler is only as good as the evidence that its IL means what the silicon
means. xdec validates at three layers, each against an independent oracle, so a
bug has to fool two unrelated implementations to survive.

| layer | question | oracle | driver |
|-------|----------|--------|--------|
| decoder | did we decode the right instruction with the right operands | capstone | `tools/diff_operands.py`, `tools/fuzz_decode.py` |
| semantics | does the IL compute what the instruction computes | Unicorn (executes the bytes) | `tools/diff_unicorn.py` |
| regressions | does a fixed bug stay fixed | frozen oracle output | `tests/corpus/*.case` |

The C++ IL interpreter (`xdec::il::Interpreter`, P5a) is what gets compared
against Unicorn, so it is itself validated by the same runs.

## Semantic differential: xdec exec vs Unicorn

`tools/diff_unicorn.py` enumerates basic blocks with capstone (splitting at
direct-branch targets, so loop-back edges do not hide inside a block), then for
every block runs several states — one all-zero edge state plus seeded random
ones — through both sides:

- **Unicorn**: maps the binary from `xdec memdump` (file bytes + zeroed bss +
  resolved relocations — the exact view the IL side seeds), plus a 64 KiB
  stack and a 64 KiB scratch region, both filled per run with splitmix64 from
  the run's seed.
- **xdec exec**: one batch invocation. The workload file carries the same
  states: full register file (x0–x30, sp, nzcv, q0–q31) and `memfill`
  directives whose seeds reproduce the identical splitmix64 byte stream on the
  C++ side. The interpreter's memory is seeded from the same image view, so
  both sides read identical bytes everywhere.

A run compares: every register, the NZCV bundle, every byte either side wrote
(byte-exact write-set comparison), the fault behaviour (both sides must fault
at the same instruction), and where control went next (branch target, taken/
fall-through of a conditional). About a third of the random registers are
biased towards plausible pointers (stack, scratch, image) so loads and stores
land somewhere mapped instead of faulting instantly.

### Known oracle differences

These are classified, not silently accepted:

- **exclusive stores** (`stxr`/`stlxr`/…): Unicorn never faults — an unmapped
  target fails the monitor and the status register reads 1. Real hardware (and
  the IL) faults on the store itself. Buckets as `oracle:exclusive-store`.
- **intrinsics** (`svc`, `bti`, `pac`/`aut`, `brk`, SIMD catch-alls,
  exclusive-load reserve/release): the spec deliberately models these as
  opaque. Buckets as `intrinsic:<name>`.

### Running it

```
python tools/diff_unicorn.py build/gcc-debug/bin/xdec.exe <binary> specs/arm64.xspec \
    --blocks 4000 --offset 0 --states 4 --seed 7 --corpus build/corpus-out
```

Throughput is roughly 8–10 blocks/second per worker (Unicorn-bound); chunk
with `--offset`/`--blocks` to parallelise. Every mismatch writes a
`case_<va>_<state>.case` to the corpus directory, with the unicorn-expected
end state already in `expect` lines — replaying it in C++ is free.

### Results so far

| binary | blocks | runs | compared | matched | fault-matched | mismatched |
|--------|--------|------|----------|---------|---------------|------------|
| libsdk_bc_lib.so (first 500, seed 1) | 500 | 2000 | 1722 | 1031 | 691 | 0 |
| libsdk_bc_lib.so (500–4500, seed 2) | 4000 | 16000 | 14455 | 8672 | 5781 | 2 → tbz bug |
| libc++_shared.so (first 1500, seed 3) | 1500 | 6000 | 4554 | 3618 | 936 | 0 |
| libAppGuard.so (first 1500, seed 4) | 1500 | 6000 | 5941 | 5626 | 315 | 0 |

The two seed-2 mismatches were one real spec bug — `tbz`/`tbnz` dropped the
`b5 << 5` high bit of the tested bit index, so bit ≥ 32 tested bit 31 instead
(a decompiler-visible bug: inverted branch conditions on sign tests). Fixed in
`specs/arm64/branch.xspec`, frozen as `corpus/case_8e6dc_*.case`.

Full-.text sweeps (every block, 3 states) run as background validation on each
spec change.

## Decoder differential and fuzzing

`tools/diff_operands.py` (from P4) compares mnemonics *and operands* against
capstone over whole executables. `tools/fuzz_decode.py` goes after the decode
space itself:

- pure random 32-bit words (mostly unallocated encodings — both sides should
  reject together);
- real instructions with 1–3 bits flipped (they live near encoding boundaries,
  where a decision-tree bug would hide).

Buckets: both-reject / both-accept (mnemonics must match, alias groups aside)
/ capstone-only (a spec hole — the interesting bucket) / xdec-only (spec
over-decodes, or capstone is older than the ISA).

```
python tools/fuzz_decode.py build/gcc-debug/bin/xdec.exe specs/arm64.xspec \
    --random 200000 --flips 200000 --seed 1 --binary <binary>
```

`xdec decode` is the batch interface: hex words on stdin, disassembly (or
`undecodable`) on stdout.

### Whole-sample coverage reports (the P5d gate)

`xdec coverage` over the four validation samples (P5d keeps these current):

| sample | words | covered | uncovered tail |
|--------|-------|---------|----------------|
| libsdk_bc_lib.so | 704,426 | 100.00 % | — |
| libc++_shared.so | 159,684 | 100.00 % | — |
| libAppGuard.so | 613,945 | 100.00 % | — |
| libsqlcipher.so | 572,144 | 99.94 % | 352: SVE encodings plus words capstone rejects too (literal-pool data in `.text`) |

"Uncovered" means no rule matched at all — catch-all sinks (`simd`,
`memory_unmodelled`) count as covered and are tracked separately in the
coverage report itself.

Coverage runs exposed one real hole beyond the catch-alls: the cache
maintenance corner of the SYS space (`dc cvau`/`dc cvac`/`dc civac`/`dc zva`,
`ic iallu`/`ic ialluis`/`ic ivau`, …) had no rules at all. The common ops are
now named rules and the rest of the op0 == 01 space (at/tlbi included) falls
to `sys`/`sysl` catch-alls — all opaque intrinsics, since cache ops change no
architectural state a decompilation should model.

### Fuzz campaign results (400k words, seed 11)

200k pure-random words + 200k real instructions with 1–3 bits flipped:

| bucket | count | meaning |
|--------|-------|---------|
| both-accept | 200,187 | mnemonics all agree (alias groups aside) |
| both-reject | 113,722 | unallocated encodings rejected together |
| capstone-only | 11,947 | spec holes: SVE/SME, MTE (`addg`), named sysregs — none in the modelled ISA |
| xdec catch-all, capstone rejects | 57,818 | deliberately over-broad sinks claim reserved encodings; opaque, never fabricated semantics |
| xdec-only (real over-decode) | 0 | every one found by earlier rounds is fixed |
| mnemonic mismatches | 0 | after alias groups |

The over-decodes the fuzzer found (all fixed, all pinned in
`test_spec_engine.cpp: fuzz-found over-decodes stay undecodable`):

1. **32-bit shifted-register forms** accepted `imm6 >= 32` (`eor w24, …, lsr #35`).
2. **add/sub shifted-register forms** accepted the reserved `shift == ROR`.
3. **logical immediates** accepted the reserved encodings (manual
   `DecodeBitMasks`: `len < 1` and `s == levels`), printing an all-ones mask.
4. **32-bit bitfield forms** (`ubfm`/`sbfm`/`bfm` and aliases) accepted
   `immr`/`imms >= 32` (`ubfx w12, w6, #35, #14`).
5. **writeback load/store** accepted `Rn == Rt(/Rt2)` (`ldr w6, [x6]!`).
6. **load-pair** accepted `Rt == Rt2`, integer and SIMD (`ldp x0, x0, [x1]`).
7. **`smulh`/`umulh`** pinned the architecturally-ignored `Ra` field to 31,
   under-decoding real encodings.
8. **PAC data-processing** catch-all accepted opcodes 8–31; only
   pacia/pacib/pacda/pacdb/autia/autib/autda/autdb exist (now eight rules).

## Corpus: frozen bug reports

`corpus/*.case` files are self-contained regression tests — instruction words,
an initial state, and oracle-expected outcomes, no binary needed:

```
base 0xVA                      where the words live
words 0xW 0xW ...              the basic block's instructions
reg x0 0x...                   initial register (up to 128 bits)
memfill 0xADDR 0xSIZE 0xSEED   splitmix64-filled region (see support/prng.h)
mem 0xADDR deadbeef            explicit bytes
expect reg x0 0x...            final register
expect mem 0xADDR deadbeef     final memory bytes
expect pc 0x...                where control went
expect fault 0x...             an unmapped access at this instruction
```

`tests/corpus/test_corpus.cpp` replays every `.case` in-process through
`liftBasicBlock` + the interpreter, as part of `xdec_tests`. Workflow for a new
bug: the differential writes the case → copy it into `corpus/` (suite goes red)
→ fix the spec/interpreter (suite goes green). Because the expects come from
the oracle, a passing replay *is* the fix's proof.

The splitmix64 stream is shared three ways and must stay in sync:
`include/xdec/support/prng.h` (C++), `splitmix64_stream` in
`tools/diff_unicorn.py` (Python), and the workload `memfill` handling in
`src/tools/xdec_main.cpp`.
