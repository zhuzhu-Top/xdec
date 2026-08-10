# 11 — The helpers header

Two different things get printed when the emitter reaches an operation C has
no operator for: bit rotate has one portable, fully-defined meaning, and an
overflow-exact condition code is a handful of comparisons on values already in
hand — both can be spelled once, correctly, for any input. Count-leading-zero
at zero, a target intrinsic, an unresolved flag condition cannot: their answer
depends on the target or on hardware this project does not model, so printing
one would be a guess wearing the clothes of a fact. `xdec_helpers.h` is the
line between them: real definitions for the first kind, declarations only for
the second.

| kind | example | in `xdec_helpers.h` |
|------|---------|---------------------|
| portable semantics | `rotr32`, `bswap64`, `cc_lt32` | `static inline`, full body |
| embedder stub | `xdec_clz32`, `xdec_mulhiu64`, `xdec_fadd32` | declared, not defined |
| per-body declaration | `__xdec_intrin_*`, `__xdec_syscall` | not in the header at all |

## Why a header instead of inline definitions

Before this, every decompiled `.c` that used a rotate carried its own copy of
`static inline uint32_t __xdec_rotr32(...) { ... }` — one `xdec` process, one
function, but the same four lines repeated in every file that happened to call
it. A function with hundreds of calls (an obfuscator's MBA expressions lean on
rotate constantly) also spelled `__xdec_rotr32` and `__builtin_bswap32` at
every use, which is long enough to compete with the arithmetic around it for
the reader's attention and, for `__builtin_bswap32`, not portable outside
GCC/Clang at all.

`xdec_helpers.h` fixes both: one `#include`, defined once, and short names —
`rotr32`, `bswap64`, `cc_lt32` — that read as arithmetic rather than as calls
into the compiler's private namespace.

## Naming

Two prefixes, and they mean two different promises:

- **No prefix** (`rotr32`, `bswap32`, `popcount64`, `cc_lt32`, ...): xdec
  defines these, in the header, the same way for every target. Calling one is
  exactly as trustworthy as calling `memcpy`.
- **`xdec_` prefix** (`xdec_clz32`, `xdec_mulhiu64`, `xdec_flagbit`,
  `xdec_fadd32`, `xdec_flagcond_stub`, ...): xdec only declares these. An
  embedder links a definition in, and what it should return is exactly what
  the comment beside each declaration says (`clz` at a zero input, `mulhi`'s
  exact semantics, the float op's rounding).

Two further names keep their old, doubled-underscore spelling and are **not**
in the header: `__xdec_intrin_*` is a family of differently-named calls, one
per instruction (`__xdec_intrin_aarch64.something`), so no fixed prototype
could cover it; `__xdec_syscall(long nr, ...)` is declared on demand in the
preamble the same way it always was, because that declaration predates this
header and a single variadic prototype was never the problem this change set
out to fix. Both keep the double underscore precisely so a reader can tell
"embedder API, unchanged" apart from "embedder API, moved here" at a glance.
`__xdec_unimplemented` is rarer still: it is never declared anywhere in the
generated output, on either side of this change — an embedder supplies it
however it likes.

## What decompiled output looks like now

```c
#include <stdint.h>
#include <stdbool.h>
#include "xdec_helpers.h"

uint32_t sub_2a2428(uint32_t* a0, uint32_t* a1) {
  ...
  _cse21 = ((_cse11 + _cse8) + ((rotr32(_cse12, 0x7) ^ rotr32(_cse12, 0x12)) ^ (_cse12 >> 0x3)));
  _cse31 = bswap32(t38);
  ...
}
```

The `#include` only appears when the body actually used something the header
defines or declares — `COptions::helpersHeader` names the path (default
`"xdec_helpers.h"`), and an empty string suppresses it entirely for a caller
who wants those names some other way. `xdec.exe`'s build copies the header
next to the executable, so pointing a compiler at a decompiled `.c` sitting
beside it just works; `--helpers-header <path|none>` overrides the path the
CLI's own `decompile` command writes.

## Coverage

`tests/emit/test_c_helpers_header.cpp` checks the `#include` gate itself: no
header-backed helper used means no include, a rotate or a byte swap or an
embedder stub each trigger it on their own, an unknown syscall alone does
not, and `COptions::helpersHeader` can be overridden or cleared. `tests/emit/
test_c_expr.cpp` and `test_c_printer.cpp` check the call sites' own spelling
(`rotr64`, `xdec_ctz64`, `cc_lt32`, ...). `eval/manifest.json`'s `eval_rotr32`
case and `samples/manifest.json`'s `sample_core_mba` case (`0x2a2428`, the
MBA-heavy function this change was written for) keep the short names honest
against real decompiles.
