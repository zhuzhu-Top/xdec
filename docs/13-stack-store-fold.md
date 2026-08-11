# 13 — Stack-store folding

`printOp`'s `Store` case (`emit/c_stmt.cpp`) prints every `Store` statement
unconditionally, on the same "a reader might be arbitrarily far away"
assumption `nameResultTemps` makes for a `Load`'s result (`docs/12`). Most of
the time a spill to a stack slot *is* read back somewhere. When it is not —
nothing in the whole function ever loads that slot, and its address never
escapes anywhere this analysis can still reason about — the assignment buys
nothing: `var_aa8 = _cse8;` right after `_cse8 = bswap32(t32);` says nothing
a reader needs, because `var_aa8` never appears again. This document is
about the emit-layer prepass that recognises that case and drops the
statement (and, when every write to the slot turns out dead this way, the
slot's own declaration) — see `docs/09` shape H1 for the taxonomy entry.

## Why this is not an IL pass, and why it is function-wide

Same reasoning as `docs/12`'s: the `Store` is exactly the one memory write
the machine code performs, so there is nothing to delete at the IL level,
only a printing decision. What differs from stack-load-fold is scope. A
load's redundancy is a fact about *one* reader, checked against *one*
load — the load's live range is exactly the span between the two. A store's
redundancy is a fact about the *whole rest of the function*: a spill is dead
only when **nothing anywhere** ever reads it back, not just along whatever
path a block-local scan happens to look down. Two stores to the same
never-read slot are dead together, in whatever order they run, because
nothing observes either write regardless.

## The analysis: `findDeadStackStores`

`include/xdec/analysis/stack_store_fold.h` / `src/analysis/stack_store_fold.cpp`.

For every `Store` in the function whose address classifies as a `StackSlot`
(`analysis::StackFrame`):

1. **No `Load` anywhere in the function reads the same delta.** Live or
   already folded into its reader's text by stack-load-fold (`docs/12`) —
   either way the read still happens as far as this analysis is concerned,
   only spelled differently, so it still disqualifies the slot.
2. **The slot's delta is not escaped**, per `analysis::StackEscapeMap`
   (`stack_escape.h`, see its own section below): a `Call`/`Intrinsic`/
   `Unimplemented` argument (any of which may read through a pointer this
   analysis has no way to reason about) or a value stored into some *other*
   location (which this analysis has no way to trace further) makes that
   delta ineligible — and so is every other delta `StackEscapeMap` closes
   into the same region, since a store there may be writing a field of the
   same aggregate the callee reads through the escaped pointer.
3. **The slot is not an aliased field** (`Variable::aliasBase`) — an aliased
   local's liveness is tied to its base slot's own accesses, which this
   analysis does not model.
4. **The slot is not the one `VariableTable::recover` promoted to `state`.**
   That promotion is deliberately store-only (see its own note in
   `variables.cpp`): a flattening dispatcher's real state value often lives
   in a register/phi between the spill and its use, so the slot is never
   read back at all, on purpose. Folding its store away would erase the one
   thing the promotion exists to show a reader — which stack slot is the
   dispatcher's own state — so this analysis leaves it alone even though
   nothing else here reads it either.

```mermaid
flowchart TD
  Store["Store(stackSlot)"] --> Read{"any Load anywhere\nreads this delta?"}
  Read -->|yes| Keep["leave as an ordinary assignment"]
  Read -->|no| Escape{"address escapes\n(call/intrinsic/stored value)?"}
  Escape -->|yes| Keep
  Escape -->|no| Alias{"aliased field, or the\npromoted 'state' slot?"}
  Alias -->|yes| Keep
  Alias -->|no| Dead["dead: statement and (if every\nwrite to the slot is dead) declaration both drop"]
```

## Stack Escape Regions (SER): `StackEscapeMap`

`include/xdec/analysis/stack_escape.h` / `src/analysis/stack_escape.cpp`.

Rule 2 above answers one delta at a time: is *this exact* stack address an
operand somewhere other than a Load/Store's own address. That is correct as
far as it goes, but a pointer handed to a callee is rarely a promise about
only the one byte it points at. `bc_lib`'s `sub_2f9a38` calls
`sub_2f949c(flags, &var_70)` after three stores lay out an 0x18-byte
aggregate starting at `var_70` (`var_70`'s own qword, then `var_68`, then
`var_60`); nothing in the caller ever loads any of the three back. Treating
only `var_70`'s own delta as escaped — which is all rule 2 alone can see —
made the other two stores look dead, and `findDeadStackStores` folded them
away: an aggregate initializer with two fields silently missing, and their
source values (registers like `t4` in the emitted C) looking unused.

`StackEscapeMap` is a separate analysis rather than a third clause bolted
onto rule 2's own scan, because it answers a different question. Rule 2 (a
`Point` escape, in `StackEscapeReason` terms) is "does this delta escape".
SER is "how wide is the region a callee might touch, starting from an
escaped delta" — reusable anywhere else that question comes up, without
`findDeadStackStores` growing a second responsibility.

**Region construction (`StackEscapeReason::StoreFootprint`).** For each
`Point` escape at delta `B`, the region starts at `[B, B)` and grows by
repeatedly folding in any Store in the function whose own delta `D` touches
or overlaps the region built so far (`D` in `[B, currentEnd]`), extending
the end to `D`'s own store width past `D`. A store that leaves a gap above
the current end is left out on every pass, since nothing bridges it — this
is a closure over *contiguous* stores, not "every store at or above `B`
anywhere in the function": the latter would treat any unrelated local
further up the same frame as escaped too, which rule 2's own tests guard
against staying dead-foldable.

```mermaid
flowchart LR
  B["Point escape at B\n(e.g. &var_70, passed to a call)"] --> Grow{"any Store at delta D\nwith B <= D <= end?"}
  Grow -->|yes: extend end to D + width| Grow
  Grow -->|no more matches| Region["region [B, end) -- StoreFootprint\n(or just [B, B+1) -- Point, if nothing adjoined)"]
```

Two escapes at unrelated deltas each close over their own region
independently; a gap between them belongs to neither, and stays eligible for
its own dead-store verdict same as before this analysis existed.

**Non-goal (`StackEscapeReason::CalleeType`, not yet implemented).** When
`types::TypeBinder` can resolve a call's parameter to `T*` with a known
`sizeof(T)`, a future phase can widen a `StoreFootprint` region to
`max(footprint, sizeof(T))` for a callee prototype that claims more than the
caller's own stores prove — additive to this phase, not a replacement, since
the enum and `compute()`'s single call site are the only things a new reason
needs to touch.

## Where the finding is consumed

**`CContext`'s constructor** (`emit/c_context.cpp`) calls
`findDeadStackStores` once, right after the stack-load-fold prepass, and
inserts every finding's `OpId` into `deadOps` — the same set `printOp`
already skips for a `Store` (`c_stmt.cpp`) and the same set
`StmtPrinter::printBlock`'s CSE-root collection already skips before calling
`addExprRoots` (see docs/09 shape I1 for why that ordering, not a second
mechanism, is what closes the reference-count cascade). Because
`findDeadStackStores` proves a *slot* dead, never just one of several writes
to it, every dead `Store`'s own delta is recorded in a new
`deadLocalStackDeltas` set; `c_printer.cpp`'s `declarations()` skips a local
there too, so a slot nothing assigns or reads any more gets no declaration
either.

## Tests

- `tests/analysis/test_stack_store_fold.cpp` — the safety rules directly
  against `findDeadStackStores`: an unread store is dead; a store a later
  load reads is not; a store whose address is passed to an intrinsic, or
  stored elsewhere as a value, is not; two stores to the same unread slot
  are both dead; a store to a global is left alone; the promoted `state`
  slot is left alone even with no reader; three stores forming an aggregate
  behind one call-escaped pointer (mirroring `sub_2f9a38`'s `b4`) are all
  kept, not just the escaped delta itself.
- `tests/analysis/test_stack_escape.cpp` — `StackEscapeMap` directly: a bare
  `Point` escape with no adjoining store still marks its own delta;
  contiguous stores above an escaped delta close into one region; a store
  across a gap is excluded; two unrelated escapes each close over their own
  region without bleeding into each other; a function with no escapes at
  all reports none.
- `tests/emit/test_c_printer.cpp` and `tests/emit/test_c_expr_reuse.cpp` —
  three existing fixtures that happened to construct exactly this
  now-optimized shape (a diamond's write-only merge, an assigned select, a
  dispatcher's own state-store-plus-switch-index pairing) were updated to
  read the slot back before returning, so each still tests what it was
  written to test instead of hitting the new fold as an incidental side
  effect. See each test's own comment for why.

## Metrics (`bc_lib` `0x2a2428`, 2026-08-10)

| Metric | Before Phase 1 | After Phase 1 |
|--------|---------------:|--------------:|
| Lines | 3715 | 3446 |
| `var_X = _cseN;` (shape H1/H2) | 277 | 185 |
| `_cseN = ...` (shape I3, minus I1's cascade) | 1179 | 1138 |
| Write-only locals (declared, never assigned or read) | 105 | 0 |

The 92-line drop in `var_X = _cseN;` is every H1 dead spill this pass finds
in this function; the 41-line drop in `_cseN = ...` is the I1 cascade —
`beginScope`'s existing `deadOps`-skip closing the reference-count
inflation those same dead spills caused, with no separate mechanism needed
(see `docs/09` shape I1 and `docs/14-emit-redundancy.md`'s Phase 2 entry).
The write-only-local
count reaching zero is `deadLocalStackDeltas`: once every write to a slot is
provably dead, the slot's declaration goes with it.

`eval/` baseline 96/96 and typed 36/36, `samples/` 4/4, and
`xdec_tests.exe`'s full suite all pass — see `eval/FINDINGS.md`'s dated
entry for the run log.
