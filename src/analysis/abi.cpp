#include "xdec/analysis/abi.h"

#include <algorithm>
#include <format>

#include "xdec/il/function.h"

namespace xdec::analysis {

int AbiSpec::argIndex(il::RegId root) const noexcept {
  for (std::size_t i = 0; i < intArgRegs.size(); ++i) {
    if (intArgRegs[i] == root) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool AbiSpec::isCalleeSaved(il::RegId root) const noexcept {
  if (!root.valid()) {
    return false;
  }
  if (root == stackPointer) {
    return true;
  }
  return std::find(calleeSavedRegs.begin(), calleeSavedRegs.end(), root) !=
         calleeSavedRegs.end();
}

bool AbiSpec::isCallerSaved(il::RegId root) const noexcept {
  if (!root.valid() || root == stackPointer) {
    return false;
  }
  if (std::find(callerSavedRegs.begin(), callerSavedRegs.end(), root) !=
      callerSavedRegs.end()) {
    return true;
  }
  if (!calleeSavedRegs.empty()) {
    return std::find(calleeSavedRegs.begin(), calleeSavedRegs.end(), root) ==
           calleeSavedRegs.end();
  }
  return true;
}

AbiSpec AbiSpec::aarch64(const il::RegisterFile& registers) {
  AbiSpec spec;
  spec.name = "aapcs64";
  spec.pointerBits = 64;
  spec.stackGrowthDir = -1;
  spec.stackPointer = registers.find("sp");
  spec.framePointer = registers.find("x29");
  spec.linkRegister = registers.find("x30");
  for (unsigned i = 0; i < 8; ++i) {
    if (const il::RegId r = registers.find(std::format("x{}", i)); r.valid()) {
      spec.intArgRegs.push_back(registers.rootOf(r));
    }
  }
  if (const il::RegId x0 = registers.find("x0"); x0.valid()) {
    spec.intRetRegs.push_back(registers.rootOf(x0));
  }
  for (unsigned i = 0; i <= 18; ++i) {
    if (const il::RegId r = registers.find(std::format("x{}", i)); r.valid()) {
      spec.callerSavedRegs.push_back(registers.rootOf(r));
    }
  }
  if (const il::RegId x30 = registers.find("x30"); x30.valid()) {
    spec.callerSavedRegs.push_back(registers.rootOf(x30));
  }
  for (unsigned i = 19; i <= 29; ++i) {
    if (const il::RegId r = registers.find(std::format("x{}", i)); r.valid()) {
      spec.calleeSavedRegs.push_back(registers.rootOf(r));
    }
  }
  for (unsigned i = 0; i < 32; ++i) {
    const il::RegId r = registers.find(std::format("q{}", i));
    if (!r.valid()) {
      continue;
    }
    const il::RegId root = registers.rootOf(r);
    if (i >= 8 && i <= 15) {
      spec.calleeSavedRegs.push_back(root);
    } else {
      spec.callerSavedRegs.push_back(root);
    }
  }
  return spec;
}

AbiSpec AbiSpec::defaultFor(const il::Function& function) {
  if (function.arch() == Arch::AArch64) {
    return aarch64(function.registers());
  }
  AbiSpec spec;
  spec.name = std::string{toString(function.arch())};
  spec.pointerBits = xdec::pointerBits(function.arch());
  if (spec.pointerBits == 0) {
    spec.pointerBits = 64;
  }
  const auto& rf = function.registers();
  for (il::RegId id{0}; id.asSize() < rf.size(); id = il::RegId{id.index() + 1}) {
    const il::RegisterInfo& info = rf[id];
    if (!info.isSubRegister() && info.regClass == il::RegClass::StackPointer) {
      spec.stackPointer = id;
      break;
    }
  }
  return spec;
}

}  // namespace xdec::analysis
