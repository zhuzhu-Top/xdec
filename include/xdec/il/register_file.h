// The target's register file.
//
// Described as data rather than compiled in, because the instruction semantics
// DSL declares it: adding an architecture must not require touching the IL. Sub
// registers are modelled explicitly (w0 is the low 32 bits of x0) so that a
// pass can reason about aliasing instead of guessing from names.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/il/op.h"
#include "xdec/support/result.h"

namespace xdec::il {

enum class RegClass : uint8_t {
  General,
  Float,
  Vector,
  /// The condition flag bundle. Its IL type is Type::flags(), not an integer.
  Flags,
  StackPointer,
  ProgramCounter,
  /// Reads as zero, writes discarded (AArch64 xzr/wzr).
  Zero,
  /// Anything else: system registers, thread pointers.
  Special,
};

[[nodiscard]] std::string_view toString(RegClass regClass) noexcept;

struct RegisterInfo {
  std::string name;
  /// Width in bits. Flags registers report 0: they carry Type::flags().
  unsigned bits = 0;
  RegClass regClass = RegClass::General;
  /// The register this one is a view into, invalid for full registers.
  RegId parent;
  /// Low bit offset within the parent.
  unsigned offsetInParent = 0;
  /// Writing this sub-register zeroes the remainder of the parent, which is how
  /// AArch64 w-register writes behave. Without modelling this, a 32-bit write
  /// followed by a 64-bit read produces a wrong value.
  bool zeroExtendsParent = false;

  [[nodiscard]] bool isSubRegister() const noexcept { return parent.valid(); }
  [[nodiscard]] Type type() const noexcept {
    return regClass == RegClass::Flags ? Type::flags() : Type::integer(bits);
  }
};

class RegisterFile {
 public:
  RegisterFile() = default;

  /// Adds a full register. Names must be unique.
  RegId add(std::string name, unsigned bits, RegClass regClass);

  /// Adds a view into an existing register.
  RegId addSubRegister(std::string name, RegId parent, unsigned offsetInParent, unsigned bits,
                       bool zeroExtendsParent);

  [[nodiscard]] const RegisterInfo& operator[](RegId reg) const;
  [[nodiscard]] std::size_t size() const noexcept { return registers_.size(); }
  [[nodiscard]] bool empty() const noexcept { return registers_.empty(); }

  /// Looks up by name; returns an invalid handle when absent.
  [[nodiscard]] RegId find(std::string_view name) const;
  [[nodiscard]] bool contains(RegId reg) const noexcept {
    return reg.valid() && reg.asSize() < registers_.size();
  }

  /// The full register that ultimately backs `reg`.
  [[nodiscard]] RegId rootOf(RegId reg) const;

  [[nodiscard]] std::string_view nameOf(RegId reg) const;

  [[nodiscard]] const std::vector<RegisterInfo>& all() const noexcept { return registers_; }

 private:
  std::vector<RegisterInfo> registers_;
  std::unordered_map<std::string, uint32_t> byName_;
};

}  // namespace xdec::il
