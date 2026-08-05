# 05 — Deobfuscation and the discovery loop

Obfuscated code fights two of the decompiler's foundations at once: the
expressions are inflated (MBA identities, opaque predicates) and the control
flow is hidden behind computed branches. This layer answers both, and the
driver closes the loop between them: simplification makes branches resolvable,
resolution makes new code visible, new code gets simplified.

| piece | question it answers | where |
|-------|---------------------|-------|
| obfuscation profile | what are we dealing with (flattening? MBA? how hot?) | `analysis/profile.h` |
| algebra rules | is this expression secretly simple | `passes/algebra.h` |
| image evaluation | which values can this expression take, given memory | `analysis/image_eval.h` |
| jump tables | is this branch really a switch | `analysis/jump_table.h` |
| resolve-indirect | turn computed branches into edges | `passes/resolve_indirect.h` |
| driver | lift → simplify → resolve → lift what was found | `decompile/driver.h` |

## Rules before reasoning

`passes/algebra.h` is a term rewriter with two tiers — plain algebra
(`x+0`, `x^x`, shifts by zero) and MBA identities
(`(x^y) + 2·(x&y) → x + y` and kin). Every rule is exact, and every rule is
proven by the test suite's random-binding oracle: both sides are evaluated by
`il::tryEvalConst` under thousands of random assignments, so a rule that is
wrong for even one input shape fails loudly. The engine runs bottom-up,
memoised (hash-consed DAGs make that O(1) per revisit), fixpoint per node,
depth-bounded against substituted chains.

The rewriter is wired into the existing fixpoint passes (`local-simplify`,
`ssa-optimize`) rather than being its own pass: identities fire exactly where
copy propagation and folding expose them. On the VMP reference sample this
collapses the optimised IL from 13,859 to 2,907 lines.

One subtlety it owns: SSA substitution builds **new** expression trees after
`foldConstants` ran, so algebra also constant-folds any fully-constant node it
visits (via `tryEvalConst`, never a second evaluator). Without that, a
rebuilt `trunc(const 0)` would survive and block the `and(x,0)` chain behind
it — which is exactly the chain jump-table recognition depends on.

## Answers, not guesses

`analysis/image_eval.h` evaluates an expression to a **bounded set** of
concrete values (cap 16; overflow degrades to `top` = unknown, never to a
wrong answer):

- `undef` and entry registers are `top`; `top` propagates by the operation's
  rules (`top + 3 = top`).
- `select(cond, a, b)` over an unknown condition is `a ∪ b` — the case the
  evaluator exists for. An obfuscator's two-way table choice is a two-element
  set, and both tables are in the image.
- Loads read the **binary image** through the `ByteReader`, so relocated
  pointers resolve to what the loader would place there; unmapped memory is
  `top`, never zero-fill.
- Phi loops contribute the empty set at the re-entry point, so
  `phi(0x77, phi)` evaluates to `{0x77}` instead of degrading to `top`.

`analysis/jump_table.h` then recognises the three table families the
obfuscators actually emit: pointer tables (`load(base + index*stride)`,
including the `select` over two bases), signed offset tables
(`anchor + sext(load32(base + index*stride))`), and packed small-entry offset
tables (`anchor + (extend(loadW) << k)` for 8/16/32-bit entries — a `(u16
<< 2)` table spans ±128 KiB of state blocks). Redundant cast chains the lifter
leaves (`zext:i64(zext:i32(load))`) are unwrapped during matching. The index
is deliberately not analysed — every entry is a valid target, so its value is
the obfuscator's problem, not ours.

## resolve-indirect: all-or-nothing per branch

`passes/resolve_indirect.h` (Ssa → Resolved) collects candidates from both
paths — the bounded value set first, whole-table enumeration (≤512 entries,
stopping at the first unreadable entry or implausible target) when the set is
`top`. A branch resolves only when **every** candidate lands on an existing
block; partial CFGs are worse than unresolved ones. Candidates without blocks
are not failures, they are *discoveries*, reported through
`Context::reportDiscovery` — all of them at once, so one driver round lifts a
whole table instead of one entry per round, and together with the branch's
whole candidate set, which is what lets the driver put the edge back on the
round that lifts those blocks rather than the round after (see below).

Enumeration needs an index. A match with none is a bare base — `load(g)`, one
global function pointer — and enumerating from it reads whatever the linker put
after the slot, which in `.data.rel.ro` is a run of more relocated function
pointers: every one of them plausible, so a branch with a single real target
collects several invented ones. The value set reads the slot and stops, which
is both the right answer and the path this shape already took.

## The driver: consistent functions, always

`decompile/driver.h` runs the loop. Each round re-lifts the function **fresh**
— from the entry plus every address discovered so far — so the pipeline always
sees one consistent maturity, never new Lifted blocks stitched into SSA fabric
(maturity is a ratchet; mixed functions fail verification, and stitching
register versions across the seam would be worse). It then runs the verified
pipeline to Ssa, probes resolution, and either loops on new discoveries or
finishes with the fully verified run to the requested target. Convergence is
structural (the address space is finite); the round budget exists as a
backstop, extends itself while rounds keep proving edges (up to a ceiling), and
an overflow names the table-enumeration gap rather than looping silently.

Carrying the candidate set with the discovery is what makes a chain of computed
branches cost one round per level instead of two. Without it, the round that
lifts a discovered block finds its incoming branch unresolved again and proves
the edge only *after* SSA was built — so the block spends that whole round with
no predecessors, no incoming dataflow, and therefore no way to resolve the
branch it ends with. On the L1 `JNI_OnLoad` sample (seven levels, 13 blocks)
that is the difference between 24 rounds and 8, with byte-identical output.

Two fixes the mega-samples forced, both in the same spirit: the verifier walks
expression DAGs iteratively (deep substituted chains outgrow any call stack),
and both the rewriter and the evaluator carry depth bounds — on obfuscated
input, "simplification skipped" must always beat "stack overflowed".

## Sample evidence (P8e)

| sample | protector | blocks | result |
|--------|-----------|-------:|--------|
| c66app `libsdk_bc_lib.so @0x844e0` | VMP-style, select-two-tables | 446 | **Resolved**, 2 rounds, ~16 s |
| bankmega `libAppGuard.so JNI_OnLoad` | AppGuard, MBA-heavy | 677 | **Resolved**, 1 round, ~1 s |
| ammana `libsdk_bc_lib.so @0x844e0` | mega VMP dispatcher | 22,000+ real state blocks discovered | honest stop at Resolved gate; see below |

The ammana sample's state machine spans the entire library `.text` — every
discovered block was verified to be real handler code, and the engine stayed
robust at 22k-block scale (the verifier, SCCP's use indexing, the rewriter
and the evaluator all walk deep DAGs iteratively or under depth bounds).
Whole-table enumeration (all three families) resolves its dispatcher brinds
to full target sets. What remains are the `brind val(...)` forms whose
targets are data-dependent with no static table shape; resolving those needs
bounded-state / speculative resolution (v2), and the Resolved verifier gate
reports them by address rather than papering over them. That is the correct
P8 v1 outcome: maximum sound static resolution, honest about the rest.

Honest, but on a dispatcher of that size it used to mean handing back nothing
at all: a few hundred unanswerable branches failed the gate, and the few
hundred blocks that *were* understood went in the bin with them. `xdec
decompile --allow-unresolved` keeps them. Each branch resolution cannot answer
becomes an opaque terminator naming its address and the expression it could not
evaluate, which is legal IL at Resolved — no invented edges, no relaxed gate,
just a hole the output admits to. The count is logged and, in the L1 harness, a
scored metric (`unresolved_branches`), so "how much of this function is still
missing" is a number that can only go down. The default is still to fail:
nothing quietly downgrades an answer.
