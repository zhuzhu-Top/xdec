#include "xdec/exec/state.h"

#include "../il/u128.h"

namespace xdec::exec {
namespace {

using il::U128;
using il::extract;
using il::insert;
using il::mask;
using il::zext;

}  // namespace

MachineState::MachineState(const il::RegisterFile& registers, uint64_t pc)
    : registers_(&registers), pc_(pc), cells_(registers.size()) {}

ConcreteValue MachineState::read(il::RegId reg) const {
  const il::RegisterInfo& info = (*registers_)[reg];
  if (info.regClass == il::RegClass::Zero) {
    return {};
  }
  if (info.regClass == il::RegClass::Flags) {
    return ConcreteValue{cells_[reg.asSize()].lo & 0xF, 0};
  }
  const il::RegId root = registers_->rootOf(reg);
  const ConcreteValue& cell = cells_[root.asSize()];
  unsigned offset = 0;
  for (il::RegId level = reg; level != root; level = (*registers_)[level].parent) {
    offset += (*registers_)[level].offsetInParent;
  }
  const U128 value =
      extract(U128{cell.lo, cell.hi}, (*registers_)[root].bits, offset, info.bits);
  return ConcreteValue{value.lo, value.hi};
}

Result<ConcreteValue> MachineState::read(std::string_view name) const {
  const il::RegId reg = registers_->find(name);
  if (!reg.valid()) {
    return err(DiagCode::BadFormat, "unknown register '{}'", name);
  }
  return read(reg);
}

void MachineState::write(il::RegId reg, ConcreteValue value) {
  const il::RegisterInfo& info = (*registers_)[reg];
  if (info.regClass == il::RegClass::Zero) {
    return;
  }
  if (info.regClass == il::RegClass::Flags) {
    cells_[reg.asSize()] = ConcreteValue{value.lo & 0xF, 0};
    return;
  }
  const il::RegId root = registers_->rootOf(reg);
  if (root == reg) {
    const U128 whole = mask(U128{value.lo, value.hi}, info.bits);
    cells_[root.asSize()] = ConcreteValue{whole.lo, whole.hi};
    return;
  }

  U128 current{value.lo, value.hi};
  U128 parent{cells_[root.asSize()].lo, cells_[root.asSize()].hi};
  for (il::RegId level = reg; level != root; level = (*registers_)[level].parent) {
    const il::RegisterInfo& view = (*registers_)[level];
    const unsigned parentWidth = (*registers_)[view.parent].bits;
    parent = view.zeroExtendsParent
                 ? zext(current, view.bits, parentWidth)
                 : insert(parent, current, view.offsetInParent, view.bits, parentWidth);
    current = parent;
  }
  const U128 whole = mask(parent, (*registers_)[root].bits);
  cells_[root.asSize()] = ConcreteValue{whole.lo, whole.hi};
}

Result<void> MachineState::write(std::string_view name, ConcreteValue value) {
  const il::RegId reg = registers_->find(name);
  if (!reg.valid()) {
    return err(DiagCode::BadFormat, "unknown register '{}'", name);
  }
  write(reg, value);
  return ok();
}

Result<void> MachineState::apply(const StatePatch& patch) {
  for (const RegisterPatch& update : patch.registers) {
    if (!registers_->contains(update.reg)) {
      return err(DiagCode::BadFormat, "state patch contains an invalid register");
    }
    write(update.reg, update.value);
  }
  return ok();
}

}  // namespace xdec::exec
