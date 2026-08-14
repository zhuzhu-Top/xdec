# 19 — Scatter dispatch: the decision-forest target shape

`analysis::DispatchRegion` (docs/17-dispatch-region.md) answers "how much of
this function is one flattened state machine" from table identity alone.
J1 (docs/18-architecture-optimization-plan.md §5.2) then used that identity
for exactly one thing: deferring `switchFor`'s two-way collapse so a region's
own site count stayed visible in the printed C, because at the time nothing
else showed it. This doc fixes what the printed C should actually look like
once a region turns out to have no `sharedTail` at all — libscplugin's own
shape, verified from its assembly rather than assumed — and records the
change that gets there: J1's region-membership defer now only ever protects
a region that actually votes a `sharedTail`, unconditionally, so a
sharedTail-less region always collapses to nested `if`/`else if` with no
flag required.

## 1. The physical shape, verified

`sub_1164f8` in `libscplugin.so` is, on the wire:

- **One** flattened state machine: a single jump table (`0x1e70a0`) and a
  single state slot (`sp-0x7b8`).
- **234** scatter two-way dispatch sites: each is `condition → one of two
  state constants → clamp → table read → br`, not one N-way hub.
- **141 of 234** sites are nested — reachable from another site's own
  handler within a short walk (`analysis::buildDispatchNestGraph`'s own
  search radius) — **93** independent roots, longest chain **28** deep.
- **No** region-wide `DispatchRegion::sharedTail`: no single block most of
  the region's pooled targets converge on. Handlers are short MBA
  straight-line spans, occasionally a self-dispatching spin site.

This is *not* "nested OLLVM passes" (one table, one state slot, start to
finish) and it is *not* bc_lib's `core_mba` shape either (`core_mba` is one
physical block with 162 live targets and a `sharedTail` a single natural
loop confirms — see `analysis::matchDispatcherShape`/`tryDispatcherLoop`).
Both claims were checked against the disassembly before this doc was
written, not assumed from either sample's own eval category.

## 2. What the decompiled C should look like

### 2.1 Three CFG node types

| Node | Meaning | Target C | Evidence |
|---|---|---|---|
| **Decision** | binary choice on a local variable | `if (var_X op c)` / `else if` | resolved two-way `IndirectBranch`; condition from `analysis::matchDispatchValues`, not the opaque state constant |
| **Handler** | runs a span of business logic then leaves | straight-line `{ ...; }` | the branch's own target block |
| **Join** | two or more handler paths converge | shared code, or an unavoidable `label:`/`goto` | multi-predecessor block (`analysis::DispatchJoin`) |

A region with no `sharedTail` is a **decision forest**: several
independent `Decision` trees (one per `DispatchNestGraph` root), each a
chain of nested `if`/`else if` on the obfuscator's own narrowed locals, not
one `switch (state)` masquerading as an N-way choice. Nesting `switch
(state)` inside `switch (state)` — printing the *opaque* state constant as
the discriminant instead of the local it was computed from — is wrong even
when it happens to be structurally nestable, because it hides the one thing
worth showing a reader: which real variable the obfuscator branched on.

### 2.2 Reference example (`0x117164`, libscplugin)

Four chained two-way sites over locals `var_730`/`var_8a8` now print as one
`if`/`else if` chain instead of four `switch (state)`s:

```c
if (var_730 == 0x0) {
  /* handler @0x117590 */
} else if (var_8a8 == 0x1) {
  /* handler @0x117200 */
} else if (var_8a8 == 0x2) {
  /* handler @0x1171b0 */
} else if (var_8a8 == 0x3) {
  /* handler @0x117310 */
} else {
  /* deeper MBA handler */
}
```

verified directly in `samples/build/out/sample_libscplugin.c` (this is the
default decompile shape now, §3 below) — not a hand-written illustration.

### 2.3 Explicitly not the target

```c
// Not this function's own evidence: one hub, 234 cases, no back-edge proof
while (true) { switch (state) { case 0x219: ...; /* 234 cases */ } }

// Not this either: nesting the opaque state instead of the real locals
switch (state) {
  case 0x1f4: switch (state) { ... }
}
```

### 2.4 Joins: an honest floor, not zero

A handler two or more of a region's own sites fall into is either inlined
(`Structurizer::claimOrCloneSharedCaseBody`, already existed for the
table-mode switch case; now reachable from the `if`/`else` collapse too)
when it is small and every predecessor is itself a resolved table dispatch,
or left as a `label:`/`goto` when it is not. Neither `switch == 0` nor
`goto == 0` is the target — see §4.

## 3. What changed: the region-membership defer now reads `sharedTail`

`switchFor`'s two-way collapse already prints `if (cond) A else B` for an
isolated site; J1's `minRegionSites` defer used to keep every member of a
large region as a table-mode switch regardless of *why* it was large. The
defer (`structure.cpp`'s `switchFor`, gated by `isMemberOfLargeDispatchRegion`
and `isMemberOfSharedTailRegion`) now narrows to only the regions that still
need it: one whose sites voted a real `sharedTail` (the flattened-loop shape
`tryDispatcherLoop`'s own hub machinery wants a switch for) keeps deferring;
one with no `sharedTail` — the scatter shape — collapses every site to
`if`/`else` exactly like an isolated one would. This is unconditional, not a
flag: `minRegionSites`'s all-regions-defer behaviour from before this change
is gone for good, not merely toggled off by default.

No new structuring pass was needed for the nesting itself: once a chained
site's own "keep dispatching" arm is an ordinary `if`/`else` instead of a
deferred switch, `Structurizer::claimCaseBody`'s existing recursion walks
straight into the next site and structures it the same way, rebuilding the
whole chain as nested `if`/`else if` for free.

`analysis::buildDispatchNestGraph` (`dispatch_region.h`/`.cpp`) is the
analysis half: purely descriptive (never claims a block, never changes
`switchFor`'s decision), it reports a region's own roots/depth/nested-site
count so the forest shape stays visible through `--emit-report`'s
`dispatch-regions:` diagnostic even once the printed C stops showing it as
a wall of switches.

## 4. Verified result on `sample_libscplugin`

Built with `--allow-unresolved --rounds 128`, before/after this change
(`region[0]`'s own diagnostic is unchanged either way — it is a read of the
same analysis regardless of how `switchFor` decides to print it):

```
dispatch-regions: 1 region(s), 234 site(s) total
  region[0]: table=0x1e70a0 stride=8 entryBits=64 clamp=0x2cc/0x213 sites=234 sharedTail=false
    nest: roots=93 depth=28 nested=141
```

| metric | before | after | direction |
|---|---|---|---|
| `switch` | 234 | 0 | every scatter site now collapses (§2's own goal) |
| `if` (incl. `else if`) | 140 | 533 | absorbs the collapsed sites, as chains |
| `goto` | 289 | 332 | **up, not down** — see the honest note below |
| `while (true)` | 49 | 49 | unchanged; not this change's target |
| unresolved branches | 0 | 0 | unchanged |

**Honest note on the `goto` increase**: the table-mode switch path can
attach a multi-tail join hub (`analysis::DispatchJoin`, J2e) as a shared
`Stmt::epilogue` printed once after the switch, letting every case that
falls into it end in a `Break` instead of a `goto`. The `if`/`else`
collapse path does not (yet) build that epilogue — each arm falls back
independently to `claimCaseBody`/`claimOrCloneSharedCaseBody`/`goto` — so a
hub several sites share can cost one `goto` per site instead of the
switch's single shared inline. This is the plan's own accepted floor (§2.4:
"3+ predecessors → label + goto, or one shared epilogue" — the parenthetical
is not mandatory), not a defect in the collapse itself; extending the
shared-epilogue mechanism to `If` nodes is a follow-up, not required for
this change to be correct.

Inspected directly: the `0x117164` region (§2.2) shows nested `if`/`else
if` on `var_730`/`var_8a8`, no internal `goto` back into the chain's own
head, and the handler bodies live inside their branch instead of behind a
switch case + label. The one `goto L_0x117164` remaining in the function is
a genuine back-edge from later, unrelated code re-entering the same decision
tree with new state — one of the function's own 49 natural loops, not an
artifact of the chain collapse.

## 5. Update: the goto-elimination plan closed part of the §4 gap

Both flags this doc's §3 introduced (`collapseScatterRegionSites`) and its
J2 sibling (`regionStructuring`) are now unconditional defaults with no CLI
switch at all — every optimization this doc and docs/18 describe is
always-on; see eval/FINDINGS.md's 2026-08-13 "结构化优化默认开启 + Goto 消除
方案" entry for the change itself. The follow-up §4 named but did not
attempt — extending the table-mode switch's shared-epilogue mechanism to
`If` nodes — is exactly what that plan's Phase 1 (J2e-if) did:
`Stmt::If` now carries the same `epilogue`/`mergeBlock` pair `Stmt::Switch`
already had, and `switchFor`'s 2-way collapse tries `joinHubByTail()` for
both arms before ever falling back to a bare `goto`. Phase 2 then unwound
the more common single-site `if (cond) goto M; else {WORK}` skip shape
Phase 1's pooled-hub matching does not cover, Phase 3 dropped the
`state=select(cond,...)` stores that shape's inlined arms leave duplicated,
Phase 4 restored `while (cond)` from the `while (true) { if (cond)
{continue;} else {WORK} }` remnant Phase 0's own collapse leaves behind a
loop header, and Phase 5 folded a `switchFor` bare-`goto` fallback's target
back in wherever it turned out to still be a single-reference orphan.

`sample_libscplugin` after all five phases: `goto` 332 → 273, `if` 533 → 349
(Phase 3's dead-store elimination, not new control flow), `while (true)` 49
→ 12. The §4 gap is narrower, not closed: a hub genuinely shared by three or
more sites still costs one `goto` per site rather than one shared inline —
Phase 1's `joinHubByTail` only claims a hub for the *first* `If` that reaches
it, exactly like the table-mode switch it mirrors — so §4's own floor ("3+
predecessors → label + goto, or one shared epilogue") still stands. See
eval/FINDINGS.md for the full per-phase numbers, including the smaller
`sub_b7000 @ 0xb7000` sample (`samples/manifest.json`'s
`sample_sub_b7000`) the plan's own worked examples were drawn from.
