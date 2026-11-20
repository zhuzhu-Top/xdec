# Register-VM Devirtualization Demo (`pyxdec`)

Self-contained end-to-end devirtualization pipeline built on `pyxdec` (`xdec` C++ library + Python bindings), matching [`docs/27-devirt-library-architecture.md`](../../docs/27-devirt-library-architecture.md).

## Running

```powershell
python samples/rqt/devirt_rqt.py
```

No external application binary is required. The script loads the self-contained anonymized VM image (`vm_payload.bin`, `XIMG` ELF64, 115,776 B) and its instruction-format table (`vm_fmt.json`):

1. Maps `PT_LOAD` segments and `.rela.dyn` relocations into `xdec.LayeredMemory` (marking `.rodata`, `.text`, and `.got` immutable so `const-fold-memory` folds table lookups).
2. Discovers basic-block CFGs and lifts `cs` (`0x10004000`, 333 basic blocks / 18,759 VM instructions) plus its internal helpers (`vm_init_array`, `vm_memcpy`, `vm_memset`) into `Custom64` `xdec::il::Function`s via `xdec.il.FunctionBuilder` and a 128-register `xdec.AbiSpec`.
3. Decompiles every lifted VM function via `xdec.decompile_from_il()` and writes the recovered C sources to `samples/rqt/output/`.
4. Executes both the pre-optimization `Cfg` IL and post-optimization `Vars` IL with `xdec.il.Interpreter.call(..., subfunctions=...)` and checks **bit-exact** equivalence on the reference test vector (`a4ca8e3beece0a45f3545b0421b10a4a -> 0x44704227`).
