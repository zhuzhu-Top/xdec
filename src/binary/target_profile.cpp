// inferTargetProfile (see the header for why this lives here).
#include "xdec/binary/target_profile.h"

#include "xdec/binary/image.h"

namespace xdec::binary {

TargetProfile inferTargetProfile(const BinaryImage& image) {
  TargetProfile profile;
  if (image.format() == BinaryFormat::Elf && image.arch() == Arch::AArch64) {
    profile.typePresets = {"android-ndk"};
    profile.syscallTable = "aarch64-linux";
    // Bionic's dynamic symbol table names the syscall-error accessor
    // `__errno`; the NDK header (types/presets/android-ndk.hdecl) declares it
    // under libc's public name, `__errno_location`. Both refer to the same
    // PLT stub -- the alias is what lets a name resolved off the loader's
    // relocation bind against the header's declaration (see
    // analysis::calleeThroughImportSlot and docs/10-import-resolution.md).
    profile.symbolAliases.emplace("__errno", "__errno_location");
  }
  if (image.format() == BinaryFormat::MachO && image.arch() == Arch::AArch64) {
    profile.typePresets = {"ios-sdk"};
    // Mach-O nlist entries spell every C symbol with a leading underscore
    // (`_malloc`, `_CFRelease`, ...), unlike ELF's dynamic symbol table.
    // types/presets/ios-sdk.hdecl declares its prototypes under that exact
    // spelling, so no alias table is needed here -- there is no known
    // iOS equivalent of Bionic's `__errno` vs `__errno_location` mismatch.
    // `syscallTable` is left empty: iOS user code does not reach the kernel
    // through the Linux aarch64 `svc` ABI the default table describes, and
    // a Mach-O binary's own `start()` has no `svc` instructions for a
    // syscall table to name in the first place.

    // dyld's `start()` (see docs/20-absd-entry-registers.md §4.2) saves
    // x19-x28 but never restores them before `BLR` into `LC_MAIN`, so the
    // program's own entry inherits whatever dyld last put there: x21/x22 are
    // `ADRL`s into dyld's own globals, offsets confirmed against dyld.i64 and
    // a live device (§7.4). An obfuscated entry reading one of these is
    // reading a platform fact, not an unknowable argument -- see
    // analysis/entry_reg.h.
    profile.entryRegOffsets = {
        {"x21", "dyld", 0x54000},   // ADRL sConfigBuffer
        {"x22", "dyld", 0x68310},   // ADRL _NSConcreteStackBlock
    };
    // x28 is kernel-launch residue, not a dyld leak, so it names no
    // companion at all -- measured "0" on every device this project has
    // tried (§7.4). A sidecar's own literal overrides this per binary if a
    // future device or kernel version differs.
    profile.entryRegLiterals = {{"x28", 0}};
  }
  if (image.format() == BinaryFormat::DyldCache && image.arch() == Arch::AArch64) {
    // A shared-cache-resident function is still ordinary iOS system code --
    // same headers, same calling convention -- it is simply mapped from
    // several files instead of one. No syscallTable/entryRegOffsets: code
    // living in the cache never runs as a process's own `start()`, so
    // dyld's x21/x22/x28 handoff (the Mach-O branch above) does not apply to
    // it; whatever a specific function needs from its caller is a per-call
    // fact for a sidecar (see analysis/entry_reg.h's MemorySeed), not a
    // platform-wide one.
    profile.typePresets = {"ios-sdk"};
  }
  return profile;
}

}  // namespace xdec::binary
