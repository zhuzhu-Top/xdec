# 09 — Expression reuse

An emitted statement is one C line per IL expression it prints, and the same
subexpression can legitimately need printing more than once — a shared
condition guards two arms, say. What this document is about is the
illegitimate case: the same computation, textually or structurally identical,
printed twice for no reason a reader can see, because two parts of the
pipeline that never talk to each other both had to produce it.

The taxonomy below has five shapes. Two are decidable purely from the IL and
are what `analysis::analyzeExpressionReuse` (`include/xdec/analysis/expr_reuse.h`)
measures; the other three need the structured statement tree or a
control-flow transform to see, and are called out as such.

## A. Exact duplicate across a scope boundary

The identical `ExprId` — the same node in the hash-consed expression pool,
not merely one that looks the same once printed — is a root of two print
units the emitter opens separate CSE scopes for (see `emit/c_expr.h`'s own
note on why scopes exist at all). Each scope's `beginScope` clears
`ExprPrinter`'s reference counts and materialized-text cache, so a node
referenced once in each scope is "referenced once" from either scope's own
point of view — never recognised as shared, and printed in full twice under
two different names.

The concrete case this project hit: a resolved computed branch's own
straight-line ops print as one `Block` statement, and the `switch` built from
its terminator prints right after as a separate `Switch` statement (see
`emit/structure.cpp`'s `IndirectBranch` case). When the switch's discriminant
is exactly the expression the block just stored to its dispatcher-state slot
— the ordinary shape a flattened function's state update takes — the two
statements used to open two scopes over the same node.

**Fixed for this shape** by `ExprPrinter::extendScope` plus the
`StmtPrinter::printBlock`/`printSwitch` pairing in `emit/c_stmt.cpp`: the
block's own `beginScope` call folds the paired switch's discriminant in
before any op prints, so the shared node is recognised as shared from its
first use and materialized once. An `if`/`while` condition is deliberately
never folded in this way — see section D on why that would be unsound, not
merely unimplemented.

`analyzeExpressionReuse` still reports this shape at the IL level (same
`ExprId` reachable from a block's own ops and from its terminator's
discriminant) regardless of whether the emitter has since learned to merge
that particular pair's scope. That is intentional: the report answers "does
this block's body and its own branch share a computation," which stays true
of the IL whether or not today's emitter happens to fix it, and a terminator
this project has not taught the emitter to pair (a `CondBranch`, or a
compare-chain switch) still prints the finding twice.

## B. Same value, re-derived through a different `Value`

Two *different* `ExprId`s — so hash-consing did not, and structurally cannot,
unify them — that a reader would call the same computation, because they
read from the same place through two different `Op`s. The clearest case is
two `Load`s of the same address with nothing between them that could have
written through it: nothing hash-conses a `Load` (see `il/expr.h`'s own note
on why a memory read is an `Op`, not an `Expr` — two reads at different times
may legitimately differ), so the second one is a fresh `ValueId` whose
`Value` leaf will never compare equal to the first's, no matter how the rest
of each expression tree matches.

`analyzeExpressionReuse` finds this restricted, safe-to-decide instance:
two `Load` ops in the same block reading the identical address expression,
with no `Store`/`Call`/`Intrinsic` between them to have plausibly clobbered
it. It is deliberately conservative — no alias analysis, no cross-block
reasoning — because a false positive here would point at a "fix" that
changes behaviour, and this report never does.

**Not yet fixed.** The general form (value forwarding across a load with a
provably-unclobbered address) is Phase 2 of the expression-reuse plan: a
`unify-values` pass that replaces the second `Load`'s uses with the first's
result. Nothing in this codebase's own OLLVM samples has turned out to need
it yet — the syscall-result reread that originally looked like this shape
(`sub_199214`'s `_cse0`/`_cse5`) turned out, once traced, to be the same
`ExprId` read through two scopes (shape A), not two different `Value`s at
all. The detector stays in place for the case that is a real re-`Load`.

## C. A stored discriminant recomputed for the branch that consumes it

An obfuscator's dispatcher stores its next state to a stack slot and reads
that same computation again, unmediated by the store, to decide where to
branch — the store and the branch never talk to each other in the IL, so
there is nothing wrong to detect at the `Function` level (the discriminant
and the stored value are shape A, an ordinary exact duplicate, or shape B, a
re-`Load` of the slot).

**What this turned out to be, once traced (`sub_199214`'s `var_58`/`_cse3`
pair):** not a missing analysis at all, but a bug in how shape A's own fix
(`extendScope`) interacted with `ExprPrinter::rootText`'s redundant-cast
elision. A `Store`'s value prints through `rootText`, which drops a leading
`ZExt` whose own operand is already a plain scalar integer — sound on its
own (the assignment's implicit widening already does that conversion), but
`rootText` used to take that shortcut unconditionally, straight past
whether the `ZExt` node *itself* was the one `isShared` had counted as
referenced 2+ times. When an obfuscator's clamp-and-dispatch idiom reused
that exact `ZExt` node as a `Select`/`compare` operand elsewhere in the same
scope (`sub_199214`'s `_cse4 = (0x3 < state); index = _cse4 ? 0x2 : state`),
the `Store` printed the operand's raw, unshared text while a *later* use of
the same node triggered its own late materialization — the same
computation, spelled out in full, twice, under two different names
(`var_58 = <full text>; ...; _cse3 = <same full text>;`).

**Fixed** by making `rootText`/`rootInteger` check `isShared` on the `ZExt`
node before eliding its cast: a shared `ZExt` now always materializes under
its own identity, so every reference — the `Store`, a `compare`, a
`Select` arm — agrees on the same `_cseN` name. See
`ExprPrinter::rootText` (`emit/c_expr.cpp`) and
`tests/emit/test_c_expr_reuse.cpp`'s redundant-root-ZExt case.

`analyzeExpressionReuse` still reports this shape's underlying IL sharing
(the same `ExprId` reachable from a block's own ops and its terminator's
discriminant) even after this fix, for the same reason section A's note
gives: the report is an IL-level fact, independent of whether today's
emitter prints it well. What is no longer true is that the *emitted C*
repeats the computation — `--reuse-report`'s count on this shape is not the
signal to watch; the absence of duplicate text in the actual output is.

## D. Why cross-arm duplication is not reported

Two `if` arms, or two `switch` cases, computing what looks like the same
expression is not this taxonomy's business, on purpose.
`analyzeExpressionReuse` only ever compares a block's own ops against *its
own* terminator, never one arm against a sibling arm. Two arms never both
execute on the same path through the function, so recomputing between them
costs nothing at runtime — it is not shared work, it is two mutually
exclusive computations that happen to read the same. Worse, folding them
into one CSE scope would require materializing the shared node before
either arm runs, which is exactly the "guard `#pragma once`ed a local, the
other path reads it uninitialised" failure `emit/c_expr.h` documents as the
reason scopes exist. If two arms are printing the same multi-hundred-column
expression, the fix is hoisting it above the branch in the *source*
control-flow sense (a real dominating computation the structurizer failed to
recognise as one), not widening CSE across arms that must stay independent.

## E. A resolved computed branch's own jump-table read, orphaned

Not duplication at all, in the sense the other shapes are — this is a `Load`
that ends up with nothing reading it, so printing it is not "the same
computation twice" but "a computation nothing in the output needs," which is
its own kind of noise in the same neighbourhood. Before `structure.cpp`
resolves an `IndirectBranch`, the target address it computes — table base
plus index times stride, read back out of the jump table by a `Load` right
before the branch — is the only description of where control goes. Once
`analysis::matchJumpTable` recognises that shape and the branch's terminator
becomes a table-mode `Switch` (see `emit/structure.cpp`'s `switchFor`), the
switch dispatches on the *index* alone; the load that reconstructed the full
target address from it has no reader left anywhere in the block. It is a
real memory read the target binary performs, but the reconstruction's own
switch has already made it vestigial.

`analyzeExpressionReuse` has nothing to say here — there is no second site
computing the same thing, just one site computing something nobody consumes.
Nor is it safe to fold into ordinary DCE (`passes::dceBlock` deliberately
never removes a `Load`, since a load can fault and removing it would change
observable behaviour on paths this project has no interest in re-deriving).

**Fixed** at emission time, where removing the printed statement changes no
IL: `emit::deadJumpTableLoad` (`emit/c_stmt.h`/`.cpp`) recognises the exact
`Block` immediately followed by its own resolved table-mode `Switch`, checks
that neither the switch's index expression nor any other op in the block
still reaches the load's result, and if so marks that `Load`'s `OpId` dead.
`emit::collectDeadOps` walks the whole structured tree once, up front, to
find every such pairing wherever it is nested, and the resulting set lives on
`CContext::deadOps` so both `Assembler::nameResultTemps` (declarations) and
`StmtPrinter::printBlock` (statements) skip the same ops — neither an
unassigned temporary nor a dead assignment reaches the output. See
`tests/emit/test_c_expr_reuse.cpp`'s resolved-jump-table-read case.

## Using the report

```
xdec decompile <binary> <address> --reuse-report
```

prints a count of each kind found, and one line per finding naming the
block's address and which two print units share the expression. It never
changes what is emitted — a phase above closes a finding, this only says one
exists — so running it before and after a change to `structure.cpp`,
`c_stmt.cpp`, or a pass is how that change's actual effect on a real sample
gets judged by a number instead of an eyeballed diff.
