# 07 — Syscalls

A kernel call is the one place a stripped binary is guaranteed to be talking
about something a reader already knows. `svc #0` with 64 in x8 is `write`, on
every AArch64 Linux there has ever been, and nothing an obfuscator does to the
surrounding code changes that. Recovering it costs four small pieces and buys
back the parts of a function that actually touch the outside world.

| piece | question it answers | where |
|-------|---------------------|-------|
| `svc` semantics | which registers does a syscall read and write | `specs/arm64/system.xspec` |
| syscall table | what is number 64 called, and how many arguments | `types/syscall/aarch64-linux.json` |
| recover-syscall | which syscall is *this* one, and trim to its arity | `passes/recover_syscall.h` |
| emitter | print it as a call a reader can check against the man page | `src/emit/c_stmt.cpp` |

## The ABI is modelled at lift time, not recovered later

The spec lifts `svc` to an intrinsic that **reads x8 and x0..x5 and defines
x0**:

```
insn svc {
  encoding "11010100000" imm16:16 "00001"
  semantics {
    write_gpr(0, 1,
              intrinsic_value("aarch64.svc", 64, imm(imm16, 16),
                              read_gpr(8, 1),
                              read_gpr(0, 1), read_gpr(1, 1), read_gpr(2, 1),
                              read_gpr(3, 1), read_gpr(4, 1), read_gpr(5, 1)));
  }
}
```

`imm16` stays an operand so `svc #0` and the rarer non-zero forms remain
distinguishable.

This is the decision the whole track rests on. Modelling the syscall as an
opaque event instead — an intrinsic with no operands — would be equally
faithful to the instruction and useless to everything downstream: SSA would not
see that x8 flows into it, so constant propagation would have nothing to
propagate *to*, and the number would have to be recovered later by scanning
backwards through a block for the last write to x8. That scan is a heuristic,
it is wrong across a branch, and it is wrong in exactly the code that most
needs it. Making the data dependency explicit at lift time turns the whole
question into an operand read.

There is a second, sharper version of the same problem. Even with a backwards
scan, nothing *reads* x8 after an opaque `svc`, so the `mov x8, #64` that
selected the syscall has no use and dead code elimination is entirely right to
delete it — the instruction is gone before any recovery pass looks for it.

Defining x0 matters for the same reason in the other direction: the kernel's
return value is a value, and the code that tests it for `-errno` has to be
seeing that value and not the entry register.

The cost is that a syscall in a function that never sets x5 now reads an
undefined x5. That is honest — the kernel does read it — and it shows up as a
visible argument rather than a silent omission, which is what the trimming step
below then removes once the arity is known.

## The table is data

`types/syscall/aarch64-linux.json` maps number to name, arity, argument
spellings, and whether the call returns at all:

```json
"64":  { "name": "write",  "args": ["int", "const void*", "size_t"], "ret": "ssize_t" },
"93":  { "name": "exit",   "args": ["int"], "ret": "void", "noreturn": true },
"172": { "name": "getpid", "argc": 0, "ret": "pid_t" },
```

Two levels of knowledge are distinguished, because the emitter can do more with
one than the other: an entry with `args` prints typed arguments, an entry with
only `argc` prints the right *number* of raw registers. `--syscall-table
<file|name|none>` overrides it; the default loads `aarch64-linux` if it is
there and degrades silently if it is not, because a missing data file must not
turn a decompilation into an error.

Numbers here are the AArch64 generic ones. A different architecture is a
different file, never a special case in the code — the numbering is a property
of the kernel ABI, and a table compiled into the decompiler would be quietly
wrong on the first binary from another one.

## recover-syscall: read, trim, record

The pass (Ssa → Ssa, after `ssa-optimize`) does three things and guesses at
none of them.

**Read the number.** x8 is operand one. Because copy and constant propagation
ran first, the operand in the common case *is* a constant — including through
the `mov w8, #nr` a compiler emits for small numbers and through a thunk whose
parameter was inlined. `Function::asConstantThroughCasts` looks through the
zero-extension and truncation chains that survive.

**Trim the arguments.** `write` reads three registers; x3..x5 hold whatever was
left in them. Keeping them is not merely noise: a syscall in a function that
never writes x5 reads the *entry* value of x5, so argument recovery counts six
parameters for a function that has two. Cutting the operand list to the real
arity fixes the call and the signature at once, and it has to happen before
`vars` counts anything — which is why this is a pass and not an emitter rule.

**Record what was learned.** The name goes on the op as a note, so an IL dump
says which syscall a bare number was.

Each step degrades instead of guessing. No table: nothing happens. Number not
constant (a thunk, or an obfuscator computing it): a note says so and all six
arguments stay, because with an unknown callee any register might be one.
Number constant but unlisted: the number is recorded and the arguments stay,
since the table's silence is about the table.

## What the C looks like

```c
long sys_write(int, const void*, size_t); // syscall 64, returns ssize_t or -errno

t0 = sys_write((int)(a0), (const void*)(a1), (size_t)(a2));
```

The argument spellings are looked up in the imported type database when there is
one, so a `struct timeval*` becomes a real cast only if a header defined the
tag. The declared return is `long` whatever the table's `ret` says, and the
comment carries the rest. This is not pedantry: the *libc* `write` returns -1 and sets
`errno`, the *kernel* entry point returns the negated errno itself, and code at
an `svc` site is testing for the second. Declaring `ssize_t` would describe the
function the code is not calling.

An unknown or non-constant number falls back to a helper that keeps everything
visible:

```c
long __xdec_syscall(long nr, ...);

t0 = __xdec_syscall(9999, a0, a1, a2, a3, a4, a5);
t1 = __xdec_syscall(((uint64_t)(a0)) /* x8 */, a1, ...);
```

Aggregate tags a signature mentions (`struct timeval*`) are forward-declared in
the preamble, so the cast compiles even when no header was imported. With
`--types` those same tags get real definitions instead — see
[06-type-import.md](06-type-import.md).

## The errno check, when it is hidden

Every real call site tests the return value: the kernel signals failure by
returning the negated errno, so a result in `[-4095, -1]` is an error and the
test is against `0xfffff001`. Unobfuscated, that comes out readable on its own
(`if (0 <= (int64_t)t0)`). Obfuscated, the sample does the same 32-bit test in
the *high* half — `(magic - state) << 32` compared against
`0xfffff00100000000` — which reads as a 64-bit comparison against an
unrecognisable constant.

The fix is not a syscall rule at all, and deliberately so. `matchShiftedCompare`
(`passes/algebra_idioms.h`) says that a comparison of two values shifted up by
the same amount is the comparison of the bits that survived, because `v ↦ v·2^k`
is strictly monotone on those bits. So `(x << 32) != 0xfffff00100000000` becomes
`(int32_t)x != -4095`, and the constant a reader recognises is back. It holds for
all six integer comparisons and both signednesses, it fires on ordinary
compiler output as readily as on obfuscated code, and it is proven by the same
random-binding oracle as every other rule in that file. No magic constant from
any particular SDK appears anywhere in it.

Writing that rule surfaced a bug in the constant evaluator it is checked
against: comparisons were masking their operands by the *result* width, and a
comparison's result is one bit, so `4 == 6` folded to true. Fixed in
`il/ceval.cpp`, pinned by *the constant evaluator compares whole operands, not
result widths*.

The other half of the plan's idiom work — a combined comment for the
release-monitor-plus-counter-reset gate that precedes these probes — is not
done. It needs the production sample to say what shape is actually worth
naming, and inventing one from a log excerpt would be guessing.

## Coverage

`eval/manifest.json`, category `syscall`: six cases built from real inline-asm
`svc` in `eval/corpus/source_syscall.c` — the three-argument base case, a
two-pointer call whose types the table knows, a zero-argument call that proves
trimming happened, an unlisted number, a number that arrives as a parameter,
and the `-errno` sign test every real call site performs. Unit tests split the
two halves: `tests/passes/test_recover_syscall.cpp` lifts real words and checks
what the pass concluded, `tests/emit/test_c_syscall.cpp` builds the intrinsic
and checks what the reader sees.
