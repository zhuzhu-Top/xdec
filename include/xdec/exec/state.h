// Concrete architectural state owned by an execution session.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "xdec/il/interp.h"
#include "xdec/il/register_file.h"
#include "xdec/support/result.h"

namespace xdec::exec {

using ConcreteValue = il::ConcreteValue;

struct RegisterPatch {
  il::RegId reg;
  ConcreteValue value;
};

struct StatePatch {
  std::vector<RegisterPatch> registers;
};

/// Register state independent of any one lifted block.
///
/// Cells are allocated for the complete spec register file. Sub-register
/// writes obey the same alias and zero-extension rules as il::Interpreter.
class MachineState {
 public:
  explicit MachineState(const il::RegisterFile& registers, uint64_t pc = 0);

  [[nodiscard]] const il::RegisterFile& registers() const noexcept { return *registers_; }
  [[nodiscard]] uint64_t pc() const noexcept { return pc_; }
  void setPc(uint64_t pc) noexcept { pc_ = pc; }

  [[nodiscard]] ConcreteValue read(il::RegId reg) const;
  [[nodiscard]] Result<ConcreteValue> read(std::string_view name) const;
  void write(il::RegId reg, ConcreteValue value);
  [[nodiscard]] Result<void> write(std::string_view name, ConcreteValue value);
  [[nodiscard]] Result<void> apply(const StatePatch& patch);

 private:
  const il::RegisterFile* registers_;
  uint64_t pc_ = 0;
  std::vector<ConcreteValue> cells_;
};

}  // namespace xdec::exec
