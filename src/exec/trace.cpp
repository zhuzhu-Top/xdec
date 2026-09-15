#include "xdec/exec/trace.h"

#include <format>
#include <ostream>

namespace xdec::exec {

std::string_view toString(BoundaryKind kind) noexcept {
  switch (kind) {
    case BoundaryKind::DirectCall: return "direct-call";
    case BoundaryKind::IndirectCall: return "indirect-call";
    case BoundaryKind::Syscall: return "syscall";
    case BoundaryKind::Intrinsic: return "intrinsic";
    case BoundaryKind::PageRequest: return "page-request";
  }
  return "?";
}

void CallbackTraceSink::onInstruction(const InstructionRecord& record) {
  if (instruction_) {
    instruction_(record);
  }
}

void CallbackTraceSink::onBoundary(const BoundaryRecord& record) {
  if (boundary_) {
    boundary_(record);
  }
}

void CallbackTraceSink::onBoundaryEffect(const BoundaryEffectRecord& record) {
  if (effect_) {
    effect_(record);
  }
}

void TextTraceSink::onInstruction(const InstructionRecord& record) {
  *output_ << std::format("#{} 0x{:x}: {}", record.sequence, record.pc,
                          record.disassembly.empty() ? "?" : record.disassembly);
  if (!record.registers.empty()) {
    *output_ << " regs";
    for (const RegisterDelta& delta : record.registers) {
      *output_ << std::format(" r{}={:x}:{:x}", delta.reg.asSize(),
                              delta.after.hi, delta.after.lo);
    }
  }
  if (!record.memory.empty()) {
    *output_ << " mem";
    for (const MemoryAccess& access : record.memory) {
      *output_ << std::format(" {}{}@0x{:x}", access.write ? "w" : "r",
                              access.size, access.address);
    }
  }
  *output_ << '\n';
}

void TextTraceSink::onBoundary(const BoundaryRecord& record) {
  *output_ << std::format("boundary {} pc=0x{:x} target=0x{:x}",
                          toString(record.kind), record.pc, record.target);
  if (!record.name.empty()) {
    *output_ << " " << record.name;
  }
  *output_ << '\n';
}

void TextTraceSink::onBoundaryEffect(const BoundaryEffectRecord& record) {
  *output_ << std::format("boundary-effect pc=0x{:x} next=0x{:x}",
                          record.boundary.pc, record.nextPc);
  if (!record.action.has_value()) {
    *output_ << " error=" << record.error;
  } else {
    *output_ << " action=" << static_cast<unsigned>(*record.action)
             << " regs=" << record.state.registers.size()
             << " memory=" << record.memory.size();
  }
  *output_ << '\n';
}

}  // namespace xdec::exec
