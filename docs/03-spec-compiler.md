# The spec compiler and engine

The DSL described in `02-dsl-ref.md` is source. This document covers what
happens to it afterwards: how it is compiled into a `SpecProgram`, how that
program is stored, and how the `SpecEngine` runs it to turn bytes into IL.

## Why there is a compiler at all

The checker already builds a validated AST. The engine could walk it. It does
not, for three reasons.

An AST walk chases pointers for every operand of every instruction, and lifting
a 64 KB function means running the semantics of sixteen thousand instructions.
The bytecode is a flat array of 24-byte instructions addressed by index, so a
body is a contiguous run and running it touches one cache line at a time.

The AST holds `std::string` names, `std::unique_ptr` children and source
locations. None of that can be written to a file and read back by `memcpy`. The
compiled form is index-addressed throughout — every name is an offset into one
string table, every reference is a `uint32_t` — so serialising it is a matter of
writing the arrays out in order.

Most importantly, compiling forces every question to be answered once. Which
register file view does `gpr[n].w` mean? How wide is the value that `imm(raw, 32
<< sf)` produces? What are the operand widths of this `flagdef`? Answering these
during the walk means answering them sixteen thousand times, and it means the
engine has to carry enough of the type system to answer them at all.

## The bytecode

A `Body` is a run of `Insn` in the program's shared code array, described by a
start index, a length, and how many slots it needs. Slots are a flat frame: the
compiler allocates them by high-water mark, so a body's frame size is known
statically and the interpreter can allocate it once.

Operations divide into three groups, which is the whole design in one sentence:

**Decode-time integers** — `ConstInt`, `Field`, `IntBinary`, `SExtInt` and so
on — compute `uint64_t` values from the instruction word. These run in the
interpreter itself. They are how a spec says "the shift amount is `imm6`" or
"this register is 32 bits wide when `sf` is 0".

**IL expressions** — `Imm`, `ExprBinary`, `Cast`, `FlagDef` — do not compute
anything. They build nodes in the target function's expression pool and leave an
`ExprId` in a slot. `ExprBinary` with `ExprOp::Add` does not add: it interns an
`add` node whose operands are two other expressions.

**IL effects** — `ReadReg`, `WriteReg`, `Load`, `Store`, `Branch`, `Call` —
append operations to the target block.

The split is what makes width polymorphism work. `32 << sf` is a decode-time
integer, evaluated the moment the instruction is decoded, so by the time an
`Imm` or `Cast` runs, its width is a concrete number. The IL never sees a
symbolic width, and the engine never needs the symbolic integer machinery that
the checker uses to prove the widths agree in the first place.

## Three ways to run a body

`decode` matches the instruction word against the decoder decision tree and
extracts field values. It never fails: an unrecognised word comes back as an
invalid `DecodedInsn`, because a decompiler meets undecodable bytes constantly
and refusing to continue would be worse than saying so.

`probe` runs the semantics with IL construction switched off, recording only
control-flow effects. This exists to solve an ordering problem. A branch names a
target address, but `il::Branch` wants a `BlockId`, and blocks are not known
until every branch in the function has been found. So a driver probes each
instruction to discover where blocks begin, creates them, and only then
elaborates.

Probing runs the same bytecode as elaboration rather than a separate analysis of
the spec. That is deliberate: two implementations of "where does this branch go"
would eventually disagree, and the disagreement would appear as a CFG that is
subtly wrong rather than as a crash.

`elaborate` runs the body for real, appending IL to a block. Every op it appends
carries the instruction's address.

## What the engine refuses to do

A branch target that `LiftSite::blockAt` cannot resolve is an error, not a
dropped edge. Silently omitting an edge produces a CFG that looks complete and
is not, and everything downstream — dominators, structuring, the emitted C —
inherits the lie.

An undecodable word elaborates to `Unimplemented`, which is a terminator.
Lifting it to a `Nop` would assert that the instruction has no effect, which is
false, and would let later passes propagate values across it.

A barrier or system instruction whose effect is not modelled becomes an
`Intrinsic`, not a `Nop`, for the same reason: a `Nop` is a claim, and it is the
wrong one.

A write to a register whose class is `Zero` is discarded, because that is what
the hardware does.

## Serialisation

`serialize` writes the arrays in order behind a magic number and a version.
`deserialize` bounds-checks every index it reads, because a spec blob is a file
and files get truncated, swapped and corrupted. A version mismatch is reported
by name rather than misread.

The decoder decision tree is not stored. It is rebuilt from the patterns on
load, which takes microseconds and removes a whole class of "the tree disagrees
with the patterns it was built from" bug.

The CLI can produce a blob and consume one:

```
xdec spec specs/arm64.xspec build/arm64.xspecb
XDEC_SPEC=build/arm64.xspecb xdec disasm libfoo.so 0x841d4 4
```

`loadEngine` decides between blob and source by the magic rather than the file
extension, so a stale blob cannot be fed to the parser and produce a confusing
syntax error.

## What the tests are for

The instruction words in `test_spec_engine.cpp` are real ones — what an
assembler emits for `subs x0, x1, x2`, `b.eq`, `ldr x0, [x1, #8]`. This matters
more than it sounds. A test built from the same encoding table it is testing
will happily agree with a spec that has the bit order reversed. Real words are
an independent oracle, which is the same reason P5 differentially tests the
decoder against Capstone and the semantics against Unicorn.

Three bugs found this way, all of which passed the DSL-level tests:

- A compile-time `if` whose taken branch jumped to the wrong offset, because
  jump patching subtracted the body's start index twice. It only showed up in
  the second body compiled, and only when the branch was actually taken — so
  `subs x0, ...` lifted correctly and `subs w0, ...` silently dropped its
  register write.
- Backward branch labels rendered as enormous forward ones, because the
  disassembly renderer did not sign-extend at the field's width. `probe` did,
  so the IL was right and only the text was wrong, which is the sort of thing
  that survives a long time.
- `add x29, sp, #0x20` printed as `add x29, xzr, #0x20`. The semantics used
  `read_gpr_sp` and were correct; only the template named the wrong view.

## Known gaps

The AArch64 spec covers eleven instructions, chosen to exercise the hard parts
of the DSL rather than to be useful. Real coverage is P4.

`Call` return-address handling and the link register are modelled by the spec
rather than by the engine, which is right, but no rule exercises it yet.

There is no function-level driver. `xdec lift` contains a thirty-line one to
prove the probe-then-elaborate flow works; the real one, with function
boundaries, call resolution and iteration to a fixpoint, comes later.
