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
  }
  return profile;
}

}  // namespace xdec::binary
