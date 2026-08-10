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
  // Mach-O + AArch64 (iOS) is the next platform this project targets; it
  // gets its own branch here, with its own preset and alias table, once a
  // Mach-O loader exists. Nothing else needs to change to add it.
  return profile;
}

}  // namespace xdec::binary
