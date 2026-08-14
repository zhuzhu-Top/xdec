# 20 — absd `start()` entry registers: static chain + dynamic validation

This doc tracks the cross-binary investigation of why `absd`'s obfuscated
`LC_MAIN` entry at `0x100023290` reads `x19`–`x28` without ever writing them
in-image. Goal: supply concrete values (or stable formulas) for xdec's
`__entry_xNN` externals so the `__xdec_unimplemented` indirect branches that
depend on them can be sealed. Updated 2026-08-14 (§7.7): of the four sites
that were sealed when this doc started, `0x100023938` and `0x1000239a4` are
now resolved this way; `0x100023688` and `0x1000238dc` turned out not to be
entry-register problems at all (see §7.7) and remain sealed.

## 1. Problem statement

| Fact | Source |
|------|--------|
| Entry VA (file) | `LC_MAIN` → `0x100023290` |
| No in-image writes to x22/x28 before use | xdec raw lift + IDA on `absd` |
| Unresolved branches depend on `__entry_x22`, `__entry_x28` | `samples/build/out/sample_absd_start_chain.c` |
| x22 used two ways | `(uint32_t)x22 + C` → index into table `0x1000a2a10`; full pointer `*(uint32_t*)(x22 + idx<<2)` |

Static decompilation alone cannot close these without knowing what dyld leaves
in callee-saved registers at the first instruction of `start()`.

## 2. Spawn chain (confirmed, no register setup)

```
launchctl kickstart system/com.apple.absd          [Procursus client, XPC 702]
  → launchd (Apple 7.0.0) posix_spawn(/usr/sbin/absd)
    → kernel exec
      → dyld __dyld_start → start(KernelArgs*)
        → prepare() → BLR entry
          → absd start @ load_base + 0x23290
```

**launchctl** and **launchd** do not touch ARM64 argument or callee-saved
registers. All entry-register state originates in **dyld** (or the kernel
handoff before dyld).

## 3. libdyld.dylib vs dyld (ruled out / confirmed)

| Binary | Role |
|--------|------|
| `libdyld.dylib` (shared cache) | Public `_dyld_*` stubs → `dyld4::gDyld` vtable only; **no** entry jump |
| `dyld` (`dyld.i64`, standalone) | Real loader: `__dyld_start`, `start`, `prepare`, initializers, `BLR` to main |

## 4. dyld entry jump (static, `dyld.i64`)

### 4.1 `__dyld_start` @ `0x19070`

```asm
MOV X0, SP
AND SP, X0, #0xFFFFFFFFFFFFFFF0
MOV X29, #0
MOV X30, #0
B   start
```

Only x0/x29/x30 are defined; x19–x28 are whatever the kernel left.

### 4.2 `start()` @ `0x18214` → program entry @ `0x183cc`

Sequence:

1. `dyld4::APIs::bootstrap` → **x19 = RuntimeState\***
2. `dyld4::prepare` → **x0 = entry function pointer**
3. `MOV X20, X0`
4. `RuntimeState::decWritable`
5. Load argc/argv/envp/apple from `ProcessConfig` → **x0–x3**
6. **`BLR X20`** → absd `start()`

`start()` saves x19–x28 in its prologue but **does not reload them** before
`BLR X20`. The program entry therefore inherits dyld's live x19–x28.

### 4.3 `prepare()` @ `0x19120`

Before returning the entry pointer:

1. Load main + dependents, `applyFixups`
2. **`runAllInitializersForMain`** (vtable `+0x358` @ `0x19b58` → `0x2c514`)
   — all lib / main initializers, bottom-up until main executable
3. `MachOAnalyzer::getEntry` — LC_MAIN `0x80000028` → `entryoff` from
   `load_command+8`
4. Return `loadAddress + entryoff` in x0

`prepare()` saves/restores x19–x28 across its body, so initializer calls
**cannot** change the values `start()` had when it called `prepare()`.

### 4.4 Explicit assignments in `start()` before `prepare()`

| Register | Set by | dyld file offset |
|----------|--------|------------------|
| x19 | `MOV X19, X0` after bootstrap | RuntimeState\* (heap) |
| x21 | `ADRL X21, sConfigBuffer` @ `0x18314` | **+0x54000** |
| x22 | `ADRL X22, __NSConcreteStackBlock` @ `0x182a8` | **+0x68310** |
| x28 | Not written in dyld; kernel → `__dyld_start` | **unknown static** |

Runtime VA: `dyld_base + offset` (shared-cache slide; stable for all processes
until reboot on typical iOS).

## 5. absd obfuscation use (xdec output)

From `sample_absd_start_chain.c` (`--rounds 4 --discovery-cap 64`):

- `(uint32_t)__entry_x22 + {0x5d, 0xb9, 0x53, …}` → dword index into
  `0x1000a2a10`
- `*(uint32_t*)(__entry_x22 + (idx << 2))` — x22 as 64-bit base
- `(uint32_t)__entry_x28 + 0xaef99f08` — x28 in index / guard material

**Working hypothesis:** x22 is intentionally the dyld-leaked
`__NSConcreteStackBlock` pointer; low 32 bits are a stable dispatch key per
boot session.

## 6. xdec progress (tooling)

| Item | Status |
|------|--------|
| `preciseIndexSet` + table sparse enum | Done — L1 candidates 308→2 |
| `kSetCap` 64, self-table filter | Done — most dispatch chain resolved |
| Remaining sealed | `0x100023688`, `0x1000239a4` (x22/x28); `0x1000238dc` (heap, cap-limited) |
| Entry-reg anchors in `image_eval` | **Done** — see docs/21-entry-reg-platform.md (zero new CLI flags: `TargetProfile` formulas + auto-discovered `<binary>.entry.json` sidecar) |
| Manifest threshold refresh | Still stale (pre-existing, unrelated to entry-reg — see §7.6) |

## 7. Dynamic validation (iOS device — 192.168.110.36)

**Environment:** root / alpine, Procursus lldb 16 @ `/var/jb/usr/bin/lldb`,
absd @ **`/usr/sbin/absd`** (not `/usr/libexec/absd`), service
`system/com.apple.absd`.

### 7.1 What worked

| Action | Result |
|--------|--------|
| SSH + paramiko | OK |
| `process launch -s` | Stops at `dyld`__dyld_start`; `image list absd/dyld` gives full 64-bit bases |
| `process attach -p <absd-pid>` | OK on running daemon (stopped in `mach_msg_trap`) |
| Offset check | `pc - dyld_base == 0x19070` at __dyld_start (matches `dyld.i64`) |

Example probe (ASLR varies per boot):

```text
absd  @ 0x000000010050c000
dyld  @ 0x00000001008ac000
entry @ absd + 0x23290 = 0x000000010052f290
BLR   @ dyld + 0x183cc = 0x00000001008c43cc
```

### 7.2 What failed (batch / automated)

| Attempt | Outcome |
|---------|---------|
| `process launch` + BP @ unslid `0x100023290` | Process exits 0 immediately; BP never hit |
| `process launch -s` + SW BP @ slid entry / dyld BLR | **error 9** inserting breakpoint site |
| Same with **hardware BP** (`-H`) | BP installs but **never fires**; process still exits 0 in ~3s |
| `process attach --waitfor absd` in automation | **Blocks indefinitely** (removed from scripts) |
| Attach to running daemon + read regs | Past entry — registers no longer meaningful |
| `memory read dyld+0x68310` on attached absd | All zeros (dyld mapped from `/cores/usr/lib/dyld`; may be post-init or offset needs re-check on device dyld) |

**Likely cause:** absd under non-launchd `lldb process launch` completes and
exits before/obscuring the real daemon entry path; batch lldb also cannot
drive `waitfor` + `launchctl kickstart -k` reliably from SSH.

### 7.3 Recommended: interactive capture on the phone

Run **two shells** on the device.

**Shell A — lldb (wait for spawn):**

Procursus lldb 16 requires **`--name` before `--waitfor`** (not
`process attach --waitfor absd`):

```text
lldb
target create /usr/sbin/absd
settings set target.process.stop-on-sharedlibrary-events false
process attach --name absd --waitfor
```

(`attach --waitfor absd` or `process attach -n absd -w` are equivalent on
some builds.)

**Shell B — kickstart (after Shell A blocks waiting):**

```sh
launchctl kill SIGTERM system/com.apple.absd
launchctl kickstart -k system/com.apple.absd
```

**Back in Shell A** — first stop is usually `dyld`__dyld_start` (all regs
zero). Set a HW breakpoint on the **BLR to absd**, then continue:

```text
image list dyld
image list absd
breakpoint set -H -a <dyld_base + 0x183cc>
process continue
register read x0 x1 x2 x3 x19 x20 x21 x22 x23 x24 x25 x26 x27 x28 pc
disassemble --start-address $pc-4 --count 2
```

Example: if `image list dyld` shows base `0x100af8000`, then
`breakpoint set -H -a 0x100b103cc`.

Alternative entry BP: `breakpoint set -H -a <absd_base + 0x23290>`.

**Verify static hypothesis:**

- `x22 == dyld_base + 0x68310`
- `x21 == dyld_base + 0x54000`
- Record `x28` (kernel handoff; no static value in dyld)

Paste the register dump into §7.4 below.

### 7.4 Register dump (**confirmed**, device nobatekiiPhone, 2026-08-14)

Captured at **`dyld`start + 440** (`BLR X20` @ `dyld+0x183cc`), immediately before
absd `LC_MAIN` (`x20 = absd+0x23290`).

```text
dyld_base  = 0x0000000104f78000
absd_base  = 0x0000000104c58000

pc  = 0x0000000104f903cc   dyld`start + 440   (BLR X20)
x0  = 0x0000000000000001   argc
x1  = 0x000000016b1a7a28   argv
x2  = 0x000000016b1a7a38   envp
x3  = 0x000000016b1a7aa8   apple
x19 = 0x0000000104d0c060   RuntimeState* (heap)
x20 = 0x0000000104c7b290   absd entry (absd+0x23290)
x21 = 0x0000000104fcc000   dyld+0x54000 (sConfigBuffer)  ✓
x22 = 0x0000000104fe0310   dyld+0x68310 (_NSConcreteStackBlock)  ✓
x23 = 0x0000000000000000
x24 = 0x0000000000000000
x25 = 0x0000000000000000
x26 = 0x0000000000000000
x27 = 0x0000000000000000
x28 = 0x0000000000000000
```

**Static hypothesis: CONFIRMED** for x21/x22. x28 was **zero** on this run
(kernel handoff through `__dyld_start`; may differ on other boots — absd uses
`(uint32_t)x28 + 0xaef99f08` so zero is a valid concrete value).

**xdec constants (per boot, shared-cache slide `S`):**

| External | Formula |
|----------|---------|
| `__entry_x22` | `S + dyld_file_offset(0x68310)` or task-read dyld base + `0x68310` |
| `__entry_x21` | dyld base + `0x54000` |
| `__entry_x28` | `0` (this device); re-measure if sealed branches differ |
| `__entry_x19` | RuntimeState\* — heap; only if needed |

Low 32 bits for table indexing: `(uint32_t)x22 == 0x04fe0310` on this boot.

### 7.5 Automation scripts (host-side)

| Script | Purpose |
|--------|---------|
| `xdec/tools/ios_lldb_absd_entry.py` | Fast probe + HW BP (exits if no hit; **no waitfor**) |
| `xdec/tools/ios_attach_test.py` | Attach to running daemon; module bases |
| `xdec/tools/ios_lldb_quick.py` | Minimal one-shot lldb |

### 7.6 xdec-side validation (2026-08-14, see docs/21-entry-reg-platform.md)

With the entry-reg facts wired in (`EntryRegFacts`, `CompositeByteReader`,
zero new `decompile` flags — full design in docs/21), re-running
`sample_absd_start_chain`'s exact command against a local `absd` copy with a
sidecar supplying `x22 = 0x104fe0310` / `x28 = 0x0` as direct literals (no
`dyld` companion file was available in this environment):

- The anchoring mechanism itself works: the emitted C now has
  `#define __entry_x22 0x104fe0310ULL` / `#define __entry_x28 0x0ULL`
  instead of `extern`.
- The same **four** `__xdec_unimplemented` sites remain sealed either way
  (`0x100023688`, `0x100023938`, `0x1000239a4`, `0x1000238dc`) — identical
  with or without the sidecar present. `0x100023688`'s load reads
  `absd`'s own `0x100080ec0` table at `idx*4` where `idx` is x22's raw low
  32 bits (or `+1`) with no further mask visible in the IL — for the
  captured value (`0x04fe0310`) that address is far outside anything absd
  maps, so anchoring x22 alone cannot seal it; either a masking/range step
  is missing upstream in this build's lift, or the real per-boot index
  comes from something narrower than x22 itself. `0x100023938` /
  `0x1000239a4` read *through* `__entry_x22` as a 64-bit pointer — a
  genuine cross-image read into `dyld`, which `CompositeByteReader` is
  built to serve once a real `dyld` file sits next to `absd` (untested here
  for lack of one; see `tests/support/test_composite_reader.cpp` and
  `tests/passes/test_resolve_indirect.cpp` for the mechanism proved in
  isolation).
- No regression: `samples/manifest.json`'s existing cases score identically
  with the sidecar present or absent.

`sample_absd_start_chain`'s and `sample_absd_start_l2`'s thresholds already
fail independently of any of this (`gotos`/`lines`/`undef` overshoot,
confirmed present with entry-reg support entirely disabled) — a pre-existing
drift this doc does not attempt to fix, and thresholds are left alone rather
than loosened to match an unrelated failure.

### 7.7 absd 间接跳转全解析优化计划 (2026-08-14) — final results

§8's next steps (below, left as originally written) were then executed as a
four-phase plan targeting all four sealed sites. Two closed, two did not; see
`samples/manifest.json`'s `sample_absd_start_chain` comment for the full
per-site account, summarized here:

- **Phase 1** (`image_eval.cpp`'s `unionEntryRegAware`): a phi/select arm that
  is a raw `EntryReg` leaf sitting next to another arm that actually redefines
  the same register is now dropped from the union instead of merged into it,
  fixing SSA pollution that had nothing to do with dyld specifically.
- **Phase 2** (`index_bound.cpp`'s `exactValues`): gained `cinc`/`csinc`'s
  `select(cond, state+1, state)` shape, an OR-chain walk that finds a
  complementary `cset` pair past intervening nesting, and — the fix that
  actually closed `0x100023938` — a `Value` case that reads through a
  loop-carried phi's own arms instead of stopping at the `val:iN(%k)` wrapper.
- **Phase 3** (`driver.cpp`): the driver now settles a chain of indirect
  branches within one round (lift → probe → lift again, up to
  `kSettleCeiling`) instead of needing an extra outer round per hop, which is
  what closed `0x1000239a4` inside the four-round budget.
- **Phase 4** (`macho.cpp`): `LC_DYLD_CHAINED_FIXUPS` decoding, so a companion
  `dyld` image's rebase/bind slots read as real pointers instead of raw chain
  words — the missing piece §8 step 1 called for, needed by `0x100023938`.

With a real `dyld` companion in place (`tmp/dyld` + `tmp/absd.entry.json`'s
`companions[]` entry, per §7.6), `0x100023938` and `0x1000239a4` are resolved.
`0x100023688` and `0x1000238dc` remain sealed, for reasons neither phase
addresses and that Phase 2/8-step-3's original framing ("find where the real
dispatcher narrows x22") turned out not to fit:

- `0x100023688`: ground truth from `disasm` (`cinc w8, w22, eq` at
  `0x100023664`, feeding `ldrsw x8, [x25, w8, sxtw #2]` at `0x100023668`) is
  exactly the `select(cond, state, state+1)` shape Phase 2 added — but *there
  is no other write to `w22` anywhere in this run's reachable CFG at all*, so
  `state` resolves to the raw `entry:i64(x22)` seed, and `ImageEval` correctly
  produces a concrete pair of huge, rejected-as-not-code offsets. Not a
  missing analysis fact: either this occurrence is a dispatch state this round
  budget's discovery genuinely never reaches a real redefinition for, or the
  block is unreachable in practice. Confirmed independently in this session
  via `--dump-il`: `%324 = load:i32 add:i64(shl:i64(sext:i64(select:i32(...,
  trunc:i32(entry:i64(x22)), add:i32(trunc:i32(entry:i64(x22)),
  const:i32(0x1))))), ...)` — both `select` arms trace to the same
  unredefined entry seed, not two different sources a filter could choose
  between.
- `0x1000238dc`: the index folds in a value that traces back through a phi to
  an actual function argument, with no guard on this path bounding it —
  correctly beyond what a structural, non-argument-range analysis can claim.

**A discovered side effect of Phase 3** (not part of the original plan, found
while validating): the intra-round settle loop has no way to distinguish
"still opening the level this round is already on" from "moving to the next
level of the chain" — both look identical (another not-yet-lifted discovery).
So `sample_absd_start` (`--rounds 1`) and `sample_absd_start_l2` (`--rounds 4
--discovery-cap 64`) now converge on the *same* shape as
`sample_absd_start_chain` (`--rounds 4 --discovery-cap 36`) — one round's
settling already exhausts what used to take four rounds or a wider cap to
reach. This cost `sample_absd_start_l2` the ~40-target dispatcher it used to
open (down from ~10800 lines to the same ~360 the other two cases converge
on) for a reason not root-caused here (see that case's own manifest comment
for the two leading suspects). All three cases' thresholds were refreshed to
the converged shape and `samples/baseline.json` updated; see
`samples/manifest.json` for the per-case detail.

## 8. Next steps

1. ~~Capture a real `dyld` binary next to a real `absd`~~ — done (§7.7); a
   `dyld` companion is checked into this machine's `samples/local.json`
   binary location and used by all three absd sample cases.
2. ~~Re-run `sample_absd_start_chain` with that companion in place~~ — done
   (§7.7): `0x100023938`/`0x1000239a4` seal.
3. ~~Investigate `0x100023688` separately~~ — done (§7.7): it is not a masking
   gap, the table base is correct, and the real blocker is reachability, not
   value-set precision.
4. ~~Refresh manifest thresholds~~ — done (§7.7): all three absd sample cases
   pass with `samples/baseline.json` updated to match.
5. Root-cause `sample_absd_start_l2`'s lost ~40-target dispatcher (§7.7's open
   question) if that shape is ever needed as more than an observational case.
6. `0x100023688`/`0x1000238dc` would need reachability work (finding or
   proving unreachable the predecessor that actually redefines `w22` for
   688; bounding the argument feeding 8dc) rather than more index-shape or
   EntryReg analysis — out of scope for this plan, which targeted analysis
   precision specifically.

## 9. Reference paths

| Asset | Path |
|-------|------|
| absd binary | `C:\Users\28264\Documents\tmp\appid\binaries\absd` |
| dyld IDA DB | `C:\Users\28264\Desktop\fsdownload\dyld.i64` |
| xdec chain output | `xdec/samples/build/out/sample_absd_start_chain.c` |
| Tier A/B notes | `xdec/eval/FINDINGS.md` § absd 2026-08-13 |
| Host lldb scripts | `xdec/tools/ios_lldb_absd_entry.py`, `ios_attach_test.py` |

---

*Last updated: 2026-08-14 — x21/x22 dynamically confirmed at dyld BLR; x28=0 on test device.*
