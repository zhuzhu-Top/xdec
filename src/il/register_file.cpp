#include "xdec/il/register_file.h"

#include <format>

namespace xdec::il {

std::string_view toString(RegClass regClass) noexcept {
  switch (regClass) {
    case RegClass::General:
      return "general";
    case RegClass::Float:
      return "float";
    case RegClass::Vector:
      return "vector";
    case RegClass::Flags:
      return "flags";
    case RegClass::StackPointer:
      return "stack-pointer";
    case RegClass::ProgramCounter:
      return "program-counter";
    case RegClass::Zero:
      return "zero";
    case RegClass::Special:
      return "special";
  }
  return "unknown";
}

RegId RegisterFile::add(std::string name, unsigned bits, RegClass regClass) {
  XDEC_ASSERT(!name.empty(), "register name must not be empty");
  XDEC_ASSERT(byName_.find(name) == byName_.end(), "duplicate register name");

  const auto index = static_cast<uint32_t>(registers_.size());
  RegisterInfo info;
  info.name = std::move(name);
  info.bits = bits;
  info.regClass = regClass;
  byName_.emplace(info.name, index);
  registers_.push_back(std::move(info));
  return RegId{index};
}

RegId RegisterFile::addSubRegister(std::string name, RegId parent, unsigned offsetInParent,
                                   unsigned bits, bool zeroExtendsParent) {
  XDEC_ASSERT(contains(parent), "sub-register parent must already exist");
  const RegisterInfo& parentInfo = registers_[parent.asSize()];
  XDEC_ASSERT(offsetInParent + bits <= parentInfo.bits,
              "sub-register does not fit inside its parent");

  const RegId id = add(std::move(name), bits, parentInfo.regClass);
  RegisterInfo& info = registers_[id.asSize()];
  info.parent = parent;
  info.offsetInParent = offsetInParent;
  info.zeroExtendsParent = zeroExtendsParent;
  return id;
}

const RegisterInfo& RegisterFile::operator[](RegId reg) const {
  XDEC_ASSERT(contains(reg), "register handle out of range");
  return registers_[reg.asSize()];
}

RegId RegisterFile::find(std::string_view name) const {
  const auto it = byName_.find(std::string{name});
  return it == byName_.end() ? RegId::invalid() : RegId{it->second};
}

RegId RegisterFile::rootOf(RegId reg) const {
  XDEC_ASSERT(contains(reg), "register handle out of range");
  RegId current = reg;
  // Sub-register chains are shallow (w0 -> x0), but loop rather than assume.
  while (registers_[current.asSize()].parent.valid()) {
    current = registers_[current.asSize()].parent;
  }
  return current;
}

std::string_view RegisterFile::nameOf(RegId reg) const {
  return contains(reg) ? std::string_view{registers_[reg.asSize()].name} : std::string_view{"?"};
}

}  // namespace xdec::il
