# 06 — Type import

Type recovery from machine code has a ceiling. A four-byte load through a
pointer proves there is a four-byte field there; nothing in the instruction
stream says the field is called `weight`, that the enum value 2 is
`EVAL_KIND_MAX`, or that the second parameter is a `size_t` rather than a
`uint64_t`. Those facts existed in a header at compile time and were discarded
by the compiler. If the header is available — the NDK's, a leaked SDK's, one
written by hand while reversing — importing it puts them back.

The design constraint throughout: **an imported type is evidence, not
instruction**. It says what the code was compiled against, which is usually
better than what inference could prove and occasionally in flat contradiction
with what the code does. Every place the two disagree is a decision recorded
below, and the tie-break is always the same — never emit C that misdescribes the
body, and never emit C that does not compile.

| piece | question it answers | where |
|-------|---------------------|-------|
| `TypeDatabase` | what types exist, and what is each one's shape | `types/database.h` |
| `.hdecl` parser | read a C header subset without a C compiler | `types/parse.h` |
| `TypeBinder` | which declaration, if any, describes *this* address | `types/binder.h` |
| apply-types | how many arguments does this call really have | `passes/apply_types.h` |
| `TypedVariables` | what does a call or `svc` site's own signature say about the values that fill it | `analysis/typed_variables.h` |
| emitter | the signature, the field names, the definitions to emit | `src/emit/c_context.cpp` |

## The database

Types are entries in a flat table addressed by `TypeId`, one tagged union per
entry (`TypeKind`: void, bool, int, float, pointer, array, struct, union, enum,
typedef, function). Cycles are the normal case — `struct Node { struct Node
*next; }` — so a pointer holds a `TypeId` and never a nested entry, and an
incomplete tag is a real entry that a later definition completes in place.

Declarations are separate from types: a `Declaration` is a name plus a `TypeId`
plus whether it is code or data. `int write(int, const void*, size_t)` puts a
function *type* in the table and a *declaration* named `write` beside it, which
is the distinction the binder needs — the type describes a shape, only the
declaration claims a name.

Two details that pay for themselves:

**Builtin typedefs keep their spelling.** `size_t` is registered as a typedef
of `unsigned long` with a `builtin` flag. Without the typedef the name is lost
at import and every signature says `uint64_t`; without the flag its definition
is emitted into the output, redefining what `<stdint.h>` already declared. With
both, `SizeTy` prints as `typedef size_t SizeTy;` and stops there.

**Definitions are emitted by reachability.** `formatDefinitions(roots)` walks
the transitive closure of the types a function actually mentioned, in dependency
order, with forward declarations for the tags that need them. Importing the
whole NDK and emitting one function does not produce the whole NDK.

## `.hdecl`: a C subset, parsed here

The parser reads a deliberately small language: typedefs, struct/union/enum
definitions, function prototypes, `extern` variables, pointers, arrays,
function pointers, and the fixed-width and platform integer spellings. No
preprocessor, no templates, no attributes beyond skipping them, no
`libclang` — the dependency it would add is larger than the whole decompiler,
and the last 10% of C declaration syntax does not appear in the headers that
matter here.

The declarator grammar is the part worth doing properly, because C's is
right-to-left and nested: `int (*ops[4])(char *, int)` is an array of four
pointers to functions. The parser builds that structure rather than
pattern-matching common shapes, and `TypeDatabase::declare` formats it back the
same way, which is how a round-trip test can prove the two agree.

Anything unparseable is **skipped and counted**, never fatal. A real header has
a construct out of scope somewhere in it, and refusing the whole file over one
line would make the feature unusable on exactly the inputs it exists for. The
`ParseReport` says how many were skipped and where.

Layout is computed on import (sizes, offsets, alignment, bitfield-free), so a
field offset is a number the emitter can match a load against.

`xdec types parse <header|preset>... [-o out.json] [--definitions]` runs the
parser alone, which is how a header gets checked before a decompilation depends
on it. `--types` takes the same arguments, repeatably.

## The binder: only exact symbols

A database on its own names nothing. `TypeBinder` is the only place a
declaration is connected to an address, and the rules are, strongest first:

1. **A defined symbol starting exactly at the address.** Its name is the
   program's own, so a declaration under that name describes this code.
2. **An import a relocation resolves.** A GOT slot holding `dlsym` is not
   `dlsym`, but what is called through it is.
3. **Nothing.** An address no symbol names gets no type, ever.

A symbol that merely *covers* an address is not evidence about that address. In
a stripped library most code is some offset into one large export, and borrowing
the enclosing symbol's prototype for an interior function would attach a
signature to a function that never had one. A name that is a function in the
image and data in the header (or the reverse) is also refused: that is two
different things sharing a spelling, and accepting it would print a call through
a variable.

`registerShaped` answers the other half — whether a type can be *used* where the
recovered code needs one. A scalar, pointer, enum, or decayed array can be; a
struct passed or returned by value cannot, because the body hands back registers
and declaring the struct would produce C that does not compile.

## apply-types: arity before argument recovery

Everything else here is emit-time, and one thing cannot be. Argument recovery
counts how many argument registers a call reads; with no prototype it must
assume all eight might matter, and any register the function does not set is
read as an *entry* value, which makes the enclosing function grow parameters it
does not have. Trimming at the printer would fix the call and leave the
signature wrong.

So `apply-types` (Ssa → Ssa, after `resolve-call`) trims each call's operand
list to its callee's declared parameter count, for two shapes of callee:

- a **constant target**, bound by address through the binder;
- an **entry register that is a parameter** of the enclosing function, whose
  declared type is a pointer to a function — which is how a call through a
  function-pointer argument gets an arity.

It only ever removes. A prototype declaring more parameters than the convention
attached describes a call this code does not make, and the extra arguments are
not invented. A variadic prototype trims nothing, because `printf` really does
read as many registers as its caller set.

This is also why `pass::Context` carries a `NameAt` resolver and a
`TypeDatabase` (`pass/pass.h`): the pass framework needs the binder's inputs
without depending on the types library, so `pass::SymbolName` mirrors
`types::BoundName` across that boundary.

## `TypedVariables`: propagating a call's own signature backward

Everything above answers a question at the *call site*: what does the header
say the callee takes, and how many registers does that trim the call to. It
says nothing about the value on the other end of the call — the argument
register itself stays a plain `uint64_t` wherever else the function reads it,
and the syscall table's `struct timeval*` for `gettimeofday` never reaches
the stack slot the address actually points at. `analysis::TypedVariables`
(`analysis/typed_variables.h`) is the piece that carries a signature's
evidence from where it is known — the call — to where it would change what
gets declared: the argument's `EntryReg`, the stack slot behind `sp + delta`,
a phi that merges several definitions, or the call's own result value.

It runs as a post-pipeline analysis, the same shape as `StackFrame` and
`VariableTable`, rather than as an IL pass: a `TypeId` has no business inside
the pure expression pool (see `types/type.h`), and its only consumers —
`VariableTable::applyImportedTypes` and the emitter's `CContext` — already run
after the pass manager, not inside it. Four sources feed it, all through the
same `types::TypeBinder::consistent` gate that decides whether an imported
type may stand in for what inference proved:

- a **direct call or a call through a typed function-pointer parameter** —
  the same two shapes `apply-types` binds against (`calleeOf`, mirrored
  rather than shared, so `analysis` never depends on `passes`);
- a **call through a GOT/import slot**: the target is not a constant the
  binder can look up, but the loader value the slot resolves to names an
  import (`calleeThroughImportSlot`), which is what types a call to `write`
  or `malloc` reached through the PLT in a real PIE `.so`, not just one
  resolved to a bare address;
- a **value loaded from a GOT/import slot naming a *data* symbol**: the slot
  holds the address of an `extern` global's own object, so the loaded value
  is a pointer to whatever the header declared under that name
  (`globalPointerType`) — see Known gaps for what this does and does not fix;
- the **syscall table**, once `SyscallTable::resolveTypes` has turned its
  JSON argument spellings into `TypeId`s against whatever header was
  imported (falling back to the bare string when none was).

From each typed call, a small use-def walker traces every argument and
result backward: an `EntryReg` names an argument variable directly; `sp +
delta` records a stack-frame slot (through one pointer, since a syscall's
`struct timeval*` parameter describes the slot's *contents*, not the address
handed to it); a `Select` or a loop-carried `Phi` visits every incoming
value; anything else — arithmetic, a load, an opaque previous result — is
left alone rather than guessed at, the same evidence-not-instruction rule
`TypeBinder` states elsewhere. `VariableTable::applyImportedTypes` then
layers this onto the `CType`-inferred variables (an `importedType` beside
each argument, local, and temp) and, where a stack slot's evidence is a
complete struct whose size matches the slot's own access width, promotes it
from an anonymous `uint64_t var_50` to a named `struct timeval var_50` —
folding any locals that turn out to be exactly one of its fields
(`var_58` becomes `var_50.tv_usec`, not a second disconnected variable).

A call's own result and a function's own return type follow the same
evidence: a syscall result temp declares at the callee's typed return
(`int32_t`, not the raw 64-bit register), and the enclosing function's own
`ret` is retyped to match when *every* reachable return traces to the same
already-typed value — never when one path returns a typed call's result and
another returns something else untraced, since that would let the presence
of evidence on one path override its absence on another.

**Cross-function reuse (optional).** `summaryOf(variables, typed)` exports a
finished function's own recovered signature — parameter types read off
`VariableTable`'s `a0, a1, ...` naming, return type from `TypedVariables`
itself — as a `CalleeSummary`, keyed by entry address into a
`CalleeSummaries` map `TypedVariables::recover` also accepts as input. A
caller decompiling more than one function from the same binary in one run
can feed a finished function's summary back in for whichever later functions
call it, which is what types a call to a local helper no header names. Today
this is a building block, not a batch mode: the CLI still decompiles one
function per invocation, so `summaries` is always empty and costs nothing
until something drives it.

## What the reader sees

**Signatures.** The imported prototype replaces the inferred one when every
type in it is register-shaped:

```c
int32_t eval_types_fn_ptr(EvalBinOp op, int32_t a, int32_t b)
```

against the same function with no header:

```c
uint32_t eval_types_fn_ptr(uint64_t a0, uint32_t a1, uint64_t a2, /* ...5 more */)
```

Where a type cannot be used it is reported rather than dropped —
`uint64_t /* header says EvalVec3 */ f(...)` for a struct returned by value.

**Field names.** A load through a parameter whose type is a pointer to a
complete struct, at an offset a field starts at, with a width that field's size
matches, prints as that field:

```c
t0 = v->x;
t2 = node->left;
```

All three conditions are required. An offset that lands mid-field means the code
is reading something this struct does not describe — a different version of it,
or not this struct at all — and printing the nearest field name would turn a
discrepancy into a plausible-looking lie. The same for a field read at the wrong
width.

A **pointer** field carries its type forward to the value it loaded, so the next
hop is nameable too, and the temporary is declared as the pointer the body uses
it as:

```c
struct EvalNode* t2;
t2 = node->left;
t4 = t2->weight;
```

**Casts back to integers.** A value declared as a pointer has to be cast back
wherever the body does byte arithmetic on it — `(uint64_t)(n) + 0x18` — because
`n + 24` on a `Node*` steps 24 elements and the machine code meant 24 bytes.
This applies identically to imported parameters and to imported field values.

**Indirect callees.** A computed call is cast through the imported type where
there is one, and through the machine's own answer where there is not:

```c
t0 = ((int32_t (*)(int32_t, int32_t))(uint64_t)(op))(a, b);
```

Empty parentheses (C's "nothing is said about the arguments") are used when the
arity came from an empty register set rather than from a declaration; `(void)`
only on a header's authority.

## Known gaps

- **Globals through the GOT: the slot's own declaration.** The slot is still
  named from the relocation (`ptr_g_eval_stats`) and still declared
  `uint8_t[]`, in both modes. Typing the *slot* requires declaring it as
  `EvalStats*` and simultaneously making the load through it stop being a
  dereference; doing one without the other stops the output from compiling,
  so it is not attempted. What `TypedVariables::globalPointerType` *does* type
  is the value the first load produces — if some other declaration in the
  same header already spells `EvalStats*` (a function taking one, say), the
  loaded temp is recognized as that pointer and a *second-level* field access
  through it prints as a field rather than a raw offset. `eval_types.hdecl`
  never spells `EvalStats*` anywhere else, so `eval_types_extern_global`
  still records today's behaviour unchanged — this is a real but narrow gap,
  not the whole feature.
- **Structs by value** are reported in a comment, never adopted.
- **Enum case labels** stay numeric. The switch is over a value, not over a
  declaration, and rewriting `0x1` as `EVAL_KIND_SUM` is a claim about which
  enum an integer belongs to that a parameter's type does not license for
  arbitrary arithmetic on it.
- **Field types are not propagated into arithmetic**: reading an `int32_t` field
  types nothing beyond that statement.

## Coverage

`eval/manifest.json`, category `types`: eight cases compiled by the NDK from
`eval/corpus/source_types.c` against `eval/corpus/types/eval_types.hdecl` —
which is both the header the corpus is built with and the header the decompiler
imports, so the test cannot drift from the ground truth. Every case is scored in
both modes (`run.ps1` / `run.ps1 -Typed`): the baseline expectation says what
inference proves on its own, the typed one says what the header adds. Unit
tests: `tests/types/test_hdecl_parser.cpp`, `tests/types/test_type_database.cpp`,
`tests/types/test_binder.cpp`, `tests/passes/test_apply_types.cpp` (including
its GOT/import-slot cases), `tests/emit/test_c_types.cpp`.

`TypedVariables` itself: `tests/analysis/test_typed_variables.cpp` (backward
propagation into arguments, stack slots and phis; GOT calls and GOT-loaded
globals; struct promotion via `VariableTable::applyImportedTypes`; the
`summaryOf`/`CalleeSummaries` round trip) and `tests/emit/test_c_typed_return.cpp`
(a call or `svc` result's own declared width; a function's own return type
following an agreeing typed result, and staying put when the body
disagrees). `samples/manifest.json`'s `sample_mega_dispatcher` — the
`sub_199214`-shaped case this whole plan started from — decompiles cleanly
with `--types android-ndk` added to its own `decompile_args` (not part of
`samples/run.ps1`'s default flags, which score dispatcher-resolution shape
rather than typing) and shows the promoted `struct timeval var_50`, the
syscall result declared `int32_t`, and named `.tv_sec`/`.tv_usec` field
access in place of the anonymous stack loads this plan opened with.
