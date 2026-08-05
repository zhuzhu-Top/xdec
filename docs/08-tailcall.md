# 08 — Tail calls

AArch64 spells two unrelated things the same way. `br x8` is how a switch
reaches its arm, and `br x0` is how `return f(a, b)` reaches `f` — same
instruction, same lack of a hint. The lifter cannot choose, so it emits an
indirect branch with no successors and leaves the question open.

Leaving it open has a cost, and it is not gradual. An unresolved computed branch
is a hole in the control-flow graph, so the verifier rejects it at `Resolved`
maturity and the whole decompilation fails — over a branch that was never going
to land inside this function in the first place. Three shapes that any
`-O1` build of ordinary C produces used to end there:

```c
int through_param(BinOp op, int a, int b) { return op(a, b); }        // br x0
int through_array(const BinOp *ops, int i, int a, int b) {            // ldr x4,[x0,i,lsl#3]
  return ops[i](a, b);                                               // br x4
}
int to_import(int a, int b) { return external(a, b); }                // b <plt>; br x17
```

| piece | question it answers | where |
|-------|---------------------|-------|
| ABI snapshot | what were the argument registers at the branch | `passes/ssa_construct.cpp` |
| recover-tailcall | is this branch leaving the function | `passes/recover_tailcall.h` |
| resolve-indirect | ... and if it is not, where in the function does it go | `passes/resolve_indirect.cpp` |

## The decision is about where the destination came from

Not about what the surrounding code looks like. Two facts settle it, and both
are read off the target expression:

**A jump table lives in this image.** Its base is an address the compiler wrote
into the instruction stream; only the index is data. So a destination whose
address chain mentions *any* address in this image is a computed jump, whatever
else it mentions.

**A tail call's destination is somebody else's pointer.** An argument register's
entry value, possibly loaded through — `ops[i](a, b)` reads the array the caller
passed. Nothing in this image says where that points, which is also why no table
could be enumerated and no successor found.

A third shape is decided by the loader rather than by arithmetic: a PLT thunk's
`br x17` reads a slot bound to another module. An import name and no address is
conclusive — a slot filled at load time cannot be a jump table in this image.

### Address position is what separates a base from an index

`ops[i](a, b)` compiles to `load(entry(x0) + sext(i) << 3)`, and a dispatcher
compiles to `load(0x9000 + i << 2)`. Both mention a parameter and both mention a
constant; what differs is *where*. So the walk over the target expression tracks
whether each subexpression is part of the address chain: the property survives
`add`, `sub`, `or` and the width adjustments a 32-bit index brings, and every
other operator computes a number. A constant under a shift is a stride whatever
its value, and a parameter under one is an index, not a base.

The same rule settles `fn = cond ? f : g; return fn(a, b)`, which is a `csel`
between two constants followed by `br`. Both arms are addresses in this image, so
the branch is left alone; the parameter in the *condition* does not count,
because what picked the pointer says nothing about where it points. Before that
distinction was drawn, this case — which is in the eval corpus as
`eval_indirect_binop` — was misread as a tail call.

One exemption, stated rather than fudged: a constant below the first page is
never treated as an address. A shared object's segments start at a page
boundary and a PIE's run-time base is a page address, so no object is addressed
by a number that small; what numbers that small are, in an address chain, is
field displacements. An unrelocated ELF reads as mapped at zero, so without this
every `p->field` in a destination would look like a table base.

## The arguments have to be captured before anyone can tell

By the time the target expression is simple enough to classify, the code that
set up the call is gone. `mov x3, x0; mov w0, w1; mov w1, w2; br x3` optimises
to a bare `brind entry(x0)`: the moves have no reader in this function, because
their reader is the callee, and dead code elimination is entirely right to
delete them.

So SSA construction records the argument registers' versions on any indirect
branch that is not yet resolved, exactly as it does for a call. This is the same
lesson as `svc` in [07-syscall.md](07-syscall.md), one level up: an ABI is
visible at the moment the instruction is lifted and nowhere later.

Recording claims nothing. If the branch turns out to go somewhere in this
function, `resolve-indirect` drops the snapshot when it sets the targets —
which it must, since those operands are uses, and keeping them would hold a
dispatcher's stale register writes alive and make them look like arguments to
`vars`.

## Where it runs

`recover-tailcall` is Ssa → Ssa, after `ssa-optimize` (without copy propagation
the target of `br x3` is a register read and the entry leaf is hidden) and
before two passes that then treat the result as the ordinary call it now is:

- **`resolve-call`**, which proves a target constant or describes its shape, and
  names the import behind a bound slot.
- **`apply-types`**, which trims the ABI snapshot to a prototype's arity. This is
  what turns `op(a, b, ...six more registers)` into `op(a, b)` and, with it, the
  enclosing signature.

Both notes survive: `resolve-call` appends its account of the target rather than
replacing what created the call.

## What the C looks like

Without a header, the arity is the ABI's over-approximation and the reader can
see that it is:

```c
uint64_t through_param(uint64_t a0, uint32_t a1, uint64_t a2, /* ... */) {
  /* tail call through a pointer the caller passed in; indirect call: target computed */
  t0 = ((uint64_t (*)(uint64_t, ...))a0)((uint64_t)(uint32_t)a1, (uint64_t)(uint32_t)a2, ...);
  return t0;
}
```

With `--types` and a prototype for the enclosing function, the same tail call
comes out as the source wrote it:

```c
int32_t eval_types_fn_ptr(EvalBinOp op, int32_t a, int32_t b) {
  t0 = ((int32_t (*)(int32_t, int32_t))(uint64_t)(op))(a, b);
  return t0;
}
```

A tail call to an imported function keeps the load through the slot, with the
import named in the comment, on the same reasoning `resolve-call` applies to a
`blr` through the GOT: the slot is writable, so what it holds now is a run-time
fact, not an image fact.

## What it declines, and why that is the point

Anything short of conclusive. A dispatcher whose table this pass cannot see
stays an unresolved branch and, if nothing else resolves it, still fails the
decompilation — because rewriting it into a call would replace real control flow
with a call that never happens, and a confident lie about control flow is far
more expensive to read past than a reported gap.

Two limits worth naming:

- **Callee-saved registers.** `br x19` where nothing wrote x19 is also a caller's
  value, but only argument registers are accepted. A function pointer arrives in
  x0..x7.
- **A GOT slot the loader binds to an address in this image.** That is a tail
  call too, but resolve-indirect can resolve it into a branch to a block, and it
  is left doing so rather than given a second interpretation.

The obfuscated sample in the repository (`libtarget.so`, `JNI_OnLoad`) is the
check that matters for the false-positive direction: it reaches thirteen
indirect branches, every one of them an OLLVM-style
`ldr x9, [x21, x9, lsl #3]; br x9` dispatch through a register-held table base,
and not one is rewritten. Set `XDEC_LOG=tailcall=debug` to see the reason for
each.

## Coverage

`tests/passes/test_recover_tailcall.cpp` lifts real ARM64 words and runs the
stock pipeline: the three shapes that are tail calls, the three that are not
(image-anchored table, select between two local addresses, pointer out of this
frame), the snapshot's contents, and one case carried through to `Vars` so that
a rewritten branch is known to survive resolution and argument recovery.
`tests/passes/test_ssa_construct.cpp` pins the snapshot itself. In the eval
corpus, `eval_types_fn_ptr` is a real `br x0` tail call in both modes, and
`eval_indirect_binop` is the select shape that must not be one.
