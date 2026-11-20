#include "xdec/devirt/verifier.h"

#include <format>

namespace xdec::devirt {

namespace {

void seedMemory(il::ExecMemory& mem, const LayeredByteReader* layered) {
  if (layered == nullptr) {
    return;
  }
  for (const auto& ov : layered->overlays()) {
    mem.seed(ov.baseVa,
             std::span<const std::byte>(
                 reinterpret_cast<const std::byte*>(ov.data.data()), ov.data.size()));
  }
}

[[nodiscard]] Result<uint64_t> executeOne(
    const il::Function& fn, const DiffVector& vec,
    const LayeredByteReader* layered, const analysis::AbiSpec* abi,
    uint64_t returnSentinelVa) {
  il::ExecMemory mem;
  seedMemory(mem, layered);
  if (vec.stackTop != 0) {
    std::vector<std::byte> stackZero(0x4000, std::byte{0});
    const uint64_t base = vec.stackTop > 0x2000 ? (vec.stackTop - 0x2000) : 0;
    mem.seed(base, stackZero);
  }

  il::Interpreter interp(fn, &mem);
  const auto& rf = fn.registers();
  if (abi != nullptr) {
    for (std::size_t i = 0; i < vec.args.size() && i < abi->intArgRegs.size(); ++i) {
      interp.writeRegister(abi->intArgRegs[i], il::ConcreteValue{vec.args[i], 0});
    }
    if (vec.stackTop != 0 && abi->stackPointer.valid()) {
      interp.writeRegister(abi->stackPointer, il::ConcreteValue{vec.stackTop, 0});
    }
  } else {
    for (std::size_t i = 0; i < vec.args.size() && i < 8; ++i) {
      il::RegId r = rf.find(std::format("x{}", i));
      if (!r.valid()) {
        r = rf.find(std::format("r{}", i));
      }
      if (r.valid()) {
        interp.writeRegister(r, il::ConcreteValue{vec.args[i], 0});
      }
    }
    if (vec.stackTop != 0) {
      for (il::RegId id{0}; id.asSize() < rf.size(); id = il::RegId{id.index() + 1}) {
        if (rf[id].regClass == il::RegClass::StackPointer && !rf[id].isSubRegister()) {
          interp.writeRegister(id, il::ConcreteValue{vec.stackTop, 0});
          break;
        }
      }
    }
  }

  const il::ExecOutcome out =
      interp.runFunction(fn.entryBlock(), {}, returnSentinelVa);
  if (out.stop != il::ExecStop::Return) {
    return err(DiagCode::VerifyFailure,
               "execution stopped with {} at {:#x}: {}",
               static_cast<int>(out.stop), out.va, out.detail);
  }
  if (out.returnValue.has_value()) {
    return out.returnValue->lo;
  }
  if (abi != nullptr && !abi->intRetRegs.empty()) {
    return interp.readRegister(abi->intRetRegs.front()).lo;
  }
  if (const auto x0 = rf.find("x0"); x0.valid()) {
    return interp.readRegister(x0).lo;
  }
  if (const auto r0 = rf.find("r0"); r0.valid()) {
    return interp.readRegister(r0).lo;
  }
  return uint64_t{0};
}

}  // namespace

Result<DiffReport> DiffVerifier::verify(
    const il::Function& baseline, const il::Function& candidate,
    std::span<const DiffVector> vectors, const LayeredByteReader* memory,
    const analysis::AbiSpec* abi, uint64_t returnSentinelVa) {
  DiffReport report;
  for (std::size_t i = 0; i < vectors.size(); ++i) {
    XDEC_TRY(const uint64_t baseOut,
             executeOne(baseline, vectors[i], memory, abi, returnSentinelVa));
    XDEC_TRY(const uint64_t candOut,
             executeOne(candidate, vectors[i], memory, abi, returnSentinelVa));
    if (vectors[i].expectedReturn.has_value() && baseOut != *vectors[i].expectedReturn) {
      return err(DiagCode::VerifyFailure,
                 "vector #{}: baseline returned {:#x}, expected {:#x}",
                 i, baseOut, *vectors[i].expectedReturn);
    }
    if (baseOut != candOut) {
      return err(DiagCode::VerifyFailure,
                 "vector #{}: candidate diverged from baseline ({:#x} != {:#x})",
                 i, candOut, baseOut);
    }
    ++report.vectorsChecked;
  }
  report.matched = true;
  return report;
}

Result<DiffReport> DiffVerifier::verifyAgainstExpected(
    const il::Function& fn, std::span<const DiffVector> vectors,
    const LayeredByteReader* memory, const analysis::AbiSpec* abi,
    uint64_t returnSentinelVa) {
  DiffReport report;
  for (std::size_t i = 0; i < vectors.size(); ++i) {
    if (!vectors[i].expectedReturn.has_value()) {
      return err(DiagCode::Internal,
                 "verifyAgainstExpected: vector #{} has no expectedReturn", i);
    }
    XDEC_TRY(const uint64_t got,
             executeOne(fn, vectors[i], memory, abi, returnSentinelVa));
    if (got != *vectors[i].expectedReturn) {
      return err(DiagCode::VerifyFailure,
                 "vector #{}: got {:#x}, expected {:#x}",
                 i, got, *vectors[i].expectedReturn);
    }
    ++report.vectorsChecked;
  }
  report.matched = true;
  return report;
}

}  // namespace xdec::devirt
