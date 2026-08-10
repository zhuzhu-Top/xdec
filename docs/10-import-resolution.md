# 10 — Import resolution

`bl 0x1d28a0` in `sub_199214` is a direct call, syntactically identical to a
call to a function this binary defines. It is not one: `0x1d28a0` is a PLT
stub — four instructions, `adrp`/`ldr`/`add`/`br`, that load a GOT slot the
loader fills in at load time and jump through it — and the GOT slot resolves
to `__errno`, Bionic's syscall-error accessor. Nothing that resolves a *direct*
call target (`calleeName`, `apply-types`, `TypedVariables`) looked through that
indirection, so this printed as `sub_1d28a0(...)`, an eight-argument call to a
function with no body, and the store that followed it —
`*(uint32_t*)(sub_1d28a0(...)) = -ret;`, the idiom every libc syscall wrapper
writes `errno` with — stayed exactly that opaque.

The fix is not specific to `__errno`. It is one question — *what does this
call target actually reach, once every kind of indirection between the
instruction and the callee is seen through* — asked consistently by every
piece downstream of it, and one architectural decision — *what a target
platform implies is inferred from the binary, not stacked onto the CLI as
another flag*. The rest of this document is that question's taxonomy and that
decision's shape.

| piece | question it answers | where |
|-------|---------------------|-------|
| `TargetProfile` | what platform is this binary, and what does that imply | `binary/target_profile.h` |
| `pltGotSlot` / `importNameForPltStub` | is this address a PLT stub, and what import does it forward to | `analysis/plt_stub.h` |
| `calleeThroughImportSlot` / `resolveCallee` | what does a call target — constant, PLT stub, or GOT-slot load — actually reach | `analysis/import_callee.h` |
| `CContext::calleeName` / `calleeType` | what to call it and how to type it, once emission asks the same question | `emit/c_context.h` |
| `foldableErrnoCall` | when `*accessor() = -ret` may print as one statement instead of two | `emit/c_stmt.cpp` |

## Three call shapes, one indirection each

Every call to an imported function takes one of three shapes at the machine
level. All three ultimately answer to the same GOT slot; they differ in how
many steps of indirection sit between the instruction that transfers control
and the slot the loader fills in.

| shape | example | resolved by reading | owned by |
|-------|---------|---------------------|----------|
| A. Direct PLT | `bl __errno@plt` | stub VA → `pltGotSlot` → GOT slot → `importNameAt` | `analysis/plt_stub.h` |
| B. Indirect GOT | `ldr x8, [got]; blr x8` | the `Load`'s address operand → `importNameAt` | `analysis/import_callee.h` (already handled before this document; unified into emission by it) |
| C. Tail call through either | `b <plt>; br x17` | recover-tailcall rewrites the branch into a call, then A or B resolves the result | `passes/recover_tailcall.h`, [08-tailcall.md](08-tailcall.md) |

Shape A is what `sub_199214` hit and what motivated this document: nothing
walked a *direct* call's constant target through a PLT stub. Shape B already
worked — `apply-types` and `TypedVariables` both read a `Load`'s address
operand and ask the loader what it resolves to — but as two separate, drifting
implementations, one per caller. Shape C is `08-tailcall.md`'s subject; it
reduces to A or B once the branch is a call, so it changes nothing new here
beyond being a caller of the same code.

### PLT stub decoding

An AArch64 ELF PLT stub is `adrp Xn, page(got); ldr Xm, [Xn, #off]` followed by
an `add`/`br` this project does not need to decode, because the GOT slot is
already known after the second instruction. `pltGotSlot` reads exactly those
two words, checks that the `ldr`'s base register is the `adrp`'s destination
(otherwise the second instruction is reading someone else's pointer, not the
address the first just computed), and returns the slot address. This was
already implemented once, inside `analysis/noreturn.cpp`, to recognise calls
to `__stack_chk_fail`, `abort`, and the rest of the short noreturn list — the
exact same indirection, asked for the exact same reason (a name behind a
constant call target). It is extracted to `analysis/plt_stub.h` so noreturn
detection and import resolution share one decoder instead of maintaining two.

`importNameForPltStub` is the whole answer for shape A: `pltGotSlot`, then
`MemoryFacts::loaderValueAt` on the result. No evidence — an unmapped stub
address, a shape that is not `adrp`+`ldr`, a GOT slot the loader left
unresolved — means no name, ever; a call this cannot place still prints as
`sub_<va>`, exactly as it always did. False positives are the failure mode
that matters here, not false negatives: misreading a same-image call as an
import would attach a fabricated prototype to a function that has none.

### Unifying shape B and the alias problem

`analysis/import_callee.h` is where shape B's two implementations
(`apply-types`'s and `TypedVariables`'s) became one:
`calleeThroughImportSlot` reads a `Load`'s constant address, asks
`MemoryFacts` what the loader binds it to, and binds that name against a
`TypeBinder`. `resolveCallee` is the three-way dispatch every caller actually
wants — constant address, GOT/import slot, function-pointer parameter — so
that `apply-types` (arity) and `TypedVariables` (argument/return typing) ask
the identical question of the identical target and cannot drift into
disagreeing about a call's arity.

Both paths — a PLT stub's resolved name, and a GOT slot's resolved name — pass
through one more step before they reach a `TypeBinder`: `TargetProfile`'s
alias table. Bionic's dynamic symbol table names the syscall-error accessor
`__errno`; the NDK header ([`types/presets/android-ndk.hdecl`](../types/presets/android-ndk.hdecl))
declares it under libc's public name, `__errno_location`. They are the same
function reached by the same PLT stub — the alias is what lets a name the
loader gave bind against a name a header declared, and it is data
(`TargetProfile::symbolAliases`), not a special case written into the binder:
a second platform with its own such mismatches adds entries here, not a new
code path.

## `TargetProfile`: inferred, not asked for

`--types android-ndk` on every invocation is a parameter nobody should have to
type twice. The binary already says it is an AArch64 ELF, and on the one
platform this project currently supports that fact alone decides which header
preset applies, which syscall ABI is in effect, and which symbol aliases a
loader name needs before it matches a header's own spelling.
`inferTargetProfile(image)` is the one place that inference lives:

```cpp
struct TargetProfile {
  std::vector<std::string> typePresets;              // {"android-ndk"}
  std::string syscallTable;                          // "aarch64-linux"
  std::map<std::string, std::string> symbolAliases;   // "__errno" -> "__errno_location"
};
```

`commandDecompile` calls it once, right after the image opens, and merges the
result into `typeSources`/`syscallSource` only where the user supplied
nothing — an explicit `--types` still wins, the same override relationship
[07-syscall.md](07-syscall.md) already established for the default syscall
table. `samples/manifest.json`'s `sample_mega_dispatcher` case needs no
`--types` argument for exactly this reason: the profile supplies
`android-ndk` on its own.

The struct is deliberately plain data answerable from `BinaryImage` alone —
format and architecture, nothing that requires the type system to already be
loaded — and it lives in `xdec::binary` rather than `xdec::types` so that a
consumer needing only the alias table (`calleeThroughImportSlot`) never has to
depend on the type system to ask for it. A second platform (Mach-O + AArch64,
i.e. iOS) is one more branch inside `inferTargetProfile`, with its own preset
and alias table; nothing that calls it changes.

## The errno store idiom

Once shape A resolves, `sub_199214`'s case 1 arm reads:

```c
t13 = __errno_location();
(*(uint32_t*)(t13)) = (-(t12));
```

which is correct and already a large improvement on `sub_1d28a0(...)`, but
still two statements for what libc's own headers would write as one:
`*__errno_location() = -ret;`. `emit/c_stmt.cpp`'s `foldableErrnoCall` looks,
per structured block, for exactly this pair — a `Store` whose value is a
negation and whose address is a call to a name that resolves to
`__errno_location` — and marks the call dead so `printOp`'s `Store` case
prints the fold instead of the two-line form.

The fold is declined, not attempted, unless the call's result has **no other
reader anywhere in the function** — not just the block the store is in. A
dispatcher whose cases converge on a shared exit is the case that makes the
"anywhere" matter: the call's result sits in the ABI's result register (`x0`
on AArch64), the same register a live argument can be carried forward in
across the merge, so a `Phi` at the merge block's head can read this exact
value on this exact edge (see `emit/c_stmt.h`'s `edgeCopies`) while never
appearing in the store's own block at all. `il::addExprRoots` does not know a
`Phi`'s operand shape — each operand is one value per incoming edge, not an
expression tree to walk into — so `foldableErrnoCall`'s reader scan treats a
`Phi` specially rather than deferring to `addExprRoots`, and scans every
block in the function, not only the one the candidate store is in. Missing
either of those checks reintroduced the exact bug this document is warning
about: the call's result printing as `/*unnamed-value-%N*/0` wherever the
merge still read it, because the call had already been marked dead and never
assigned a name. `tests/emit/test_c_import_call.cpp` pins both the fold and
this decline as separate cases.

## What this declines, and why

The same "no evidence, no claim" rule the rest of the type-import pipeline
follows ([06-type-import.md](06-type-import.md)):

- **An unrecognised stub shape** — anything other than `adrp`+`ldr` with
  matching registers — is not treated as a PLT stub at all. A same-image call
  whose first instruction happens to be an `adrp` for an unrelated reason is
  never misread as an import.
- **A GOT slot the loader left unresolved** — a relocation table this loader
  does not cover, or a truncated `RELR` table (see
  [`elf.cpp`](../src/binary/elf.cpp)'s existing warnings there) — resolves to
  no name, and a call through it still prints as a cast through the machine's
  own register width.
- **An alias with no entry** passes a name through unchanged. Only the
  handful of documented mismatches (today: `__errno`) get a `TargetProfile`
  entry; a name that already matches its header spelling needs none.
- **The errno fold, whenever any other read of the call's result exists.**
  Folding here would not just look different — the merge's `t0 = t13;` copy
  would read a value declared nowhere, an actively wrong program.

## Coverage

- `tests/analysis/test_plt_stub.cpp`: synthetic `adrp`/`ldr` byte pairs decode
  to the right GOT slot (including a negative page offset), a mismatched base
  register or a non-`adrp` first word is rejected, and `importNameForPltStub`
  follows a stub through to a loader-bound name.
- `tests/analysis/test_import_callee.cpp`: a GOT slot's import name binds
  against its header declaration, an unbound slot resolves to nothing, a
  loader name with no `TargetProfile` alias finds nothing, the alias makes it
  find the header's own spelling, and `resolveCallee`'s function-pointer and
  constant-address paths still answer as before.
- `tests/emit/test_c_import_call.cpp`: a direct call to a PLT-resolved import
  prints under its name (not `sub_<va>`), a known-noreturn import is
  annotated, the errno fold applies when nothing else reads the call's
  result, and — the regression this document exists to prevent — does not
  apply, with no unnamed value anywhere in the output, when a dispatcher
  merge's `Phi` also reads it.
- `samples/manifest.json`'s `sample_mega_dispatcher` (`sub_199214`, real
  Bionic binary, no `--types` argument): `__errno_location()` with a `int32_t*`
  return type, zero arguments, and the folded store, all from
  `inferTargetProfile` alone.
- `eval/`'s 36-case suite, both baseline and `--types`-loaded modes: no
  regression from any of the above.
