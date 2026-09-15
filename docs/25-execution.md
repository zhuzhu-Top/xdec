# 25 — Deterministic execution and trace

`xdec_exec` is the execution-facing half of xdec. It follows AArch64 control
flow using the same xspec semantics as the decompiler, but deliberately does
not implement an operating system, libc, JNI, ObjC, RPC, or a persistence
format.

## Ownership and boundaries

An `ExecSession` owns control-flow progress. Its `MachineState` and
`GuestAddressSpace` are the complete guest truth while it runs. There is no
implicit host/device memory coherence.

Calls, syscalls, and opaque intrinsics become `ExternalRequest` values. With no
handler, execution returns an `ExternalBoundary` result and a single-use
`ResumeToken`. An embedder may inspect the state and resume with explicit
register and memory patches. A registered `IExternalHandler` performs the same
operation inline.

External memory effects are ordered `MemoryPatch` operations:

- `write` updates mapped writable memory;
- `map` creates zero-filled pages with optional initial bytes;
- `protect` changes page permissions;
- `unmap` removes pages.

No handler receives mutable pointers into guest memory. This makes side effects
auditable and leaves a clean future seam for sandbox or remote implementations.

## Minimal use

```cpp
#include "xdec/exec/session.h"

xdec::exec::GuestAddressSpace memory;
memory.seedImage(image);

xdec::exec::MachineState state{engine.program().registers, entry};
state.write("x0", {argument, 0});

xdec::exec::VectorTraceSink trace;
xdec::exec::ExecSession session{engine, memory, std::move(state)};
session.setObserver(&trace);

const xdec::exec::ExecResult result = session.run();
```

Production users should link the library API directly. There is intentionally
no trace CLI or command-line state language.

## Trace contract

`IExecObserver` receives immutable logical records:

- `InstructionRecord`: PC, raw word, disassembly, root-register deltas, and
  memory accesses;
- `BoundaryRecord`: call/syscall/intrinsic boundary metadata;
- `BoundaryEffectRecord`: the handler action and exact state/memory patches,
  including failed responses, so execution across a boundary can be replayed.

`TracePolicy` independently enables disassembly, register deltas, memory
values, bounded context bytes, external effects, and an instruction-record
limit. Boundary records attached to the final instruction do not consume that
limit. Text and vector sinks are convenience adapters; tracedb, JSON, online
taint, or streaming are external consumers rather than execution dependencies.

## Current ExecIR

`compileExecBlock` reads executable guest bytes, decodes and elaborates them
through `SpecEngine`, then groups the resulting Lifted IL operations by source
instruction into `ExecInstruction` packets. `ExecSession` caches these blocks
against code-page generations. Any write or permission change invalidates the
affected cached block.

This first ExecIR prioritizes one semantic source and immediate usability. A
future compact interpreter or block JIT can replace its internal Lifted IL
without changing session, environment, or observer APIs.

## Explicit limitations

- execution is single-threaded;
- calls are environment boundaries rather than an invented ABI stack model;
- unsupported instructions and declined intrinsics stop explicitly;
- the first executor runs Lifted IL block by block and is correctness-oriented,
  not yet a JIT;
- page faults are only resolved by a caller-supplied `PageProvider`.

The IL interpreter remains checked by the existing Unicorn-derived corpus and
semantic differential workflow; Unicorn is not a runtime dependency.
