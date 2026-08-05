# L1 samples: xdec against real obfuscated `.so` files

`eval/` proves the decompiler against known C: every function there has a
source file, so a wrong signature or a stray `goto` is unambiguously a
defect. This directory is the next tier up -- real, obfuscated `.so` files
the decompiler does not control the source of. There is no ground truth to
compile against, so a case here is scored on *shape* only: how many gotos,
switches, and ternaries came out, and whether the decompile even completes
within its round budget. See [`../eval/FINDINGS.md`](../eval/FINDINGS.md) for
how the two tiers relate, and the plan's `Phase A` for what shape changes are
being driven by this suite.

## Running

```powershell
# NDK eval (L0) must stay green -- it is the gate this suite does not replace.
.\eval\run.ps1
.\eval\run.ps1 -Typed

# L1
.\samples\run.ps1
.\samples\run.ps1 -UpdateBaseline   # after a deliberate, reviewed change in shape
```

## Adding a binary

The `.so` files themselves are never checked into the repo. A case's
`binary` field in `manifest.json` is a key, not a path; `run.ps1` resolves it
through, in order:

1. An `XDEC_SAMPLE_<KEY>` environment variable (key upper-cased), for one-off
   or CI runs.
2. `samples/local.json` (gitignored, one per machine) -- copy
   `local.example.json` and fill in real paths.

## Adding a case

Copy the shape of an existing entry in `manifest.json`:

- `name` -- must be unique; also becomes the output filename.
- `binary` -- the `local.json` key.
- `address` -- hex string, e.g. `"0xe4c8c"`.
- `rounds` -- optional; falls back to `run.ps1 -Rounds` (default 8). The
  harness always passes it, which makes it a hard wall rather than a budget the
  driver may extend, so a case that needs more rounds than it says fails
  instead of quietly costing time. Set it explicitly only to document a known
  gap.
- `decompile_args` -- optional extra CLI arguments (`--types`,
  `--syscall-table`, `--allow-unresolved`, ...).
- `expect` -- `max_gotos`, `min_switches`, `max_ternaries`, `min_ifs`,
  `min_loops`, `max_unresolved_branches`, `max_undef`, `patterns`,
  `forbid_patterns` (see `eval/score.py` for the full set `check_case`
  understands).

A function nothing resolves whole is still worth a case. Run it with
`--allow-unresolved` and bound `max_unresolved_branches`: the decompile then
succeeds with each unanswerable computed branch marked in place, and the count
of those marks is the metric progress shows up in. Budget `max_undef` alongside
it -- blocks whose only predecessors were sealed away lose their incoming
dataflow, and the undef reads that follow are the cost of the seal, not a
separate bug. Both default to zero, so an ordinary case cannot pick them up by
accident.

**Increment slowly.** Per the plan, add at most one new L1 case per completed
Phase A sub-step -- this suite is meant to be spot-checked by a human, not
grown into a second corpus. When a pass improvement lowers a case's
goto/ternary count, tighten `expect` to match and re-run `-UpdateBaseline` so
the new number is the floor, not a one-time win.

## Relationship to `finetuning`

The `finetuning` project's MCP/decomp tooling uses these same `.so` files
(e.g. `libtarget.so`) to produce *training* data -- traced CFGs, manual
annotations. This suite only cares about xdec's own `decompile` CLI output;
it does not require or produce a hand-built CFG.
