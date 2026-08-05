// Effectful operations.
//
// Everything that is not a pure function of its operands is an Op: register and
// memory access, control flow, calls, and operations whose effect is known only
// by name. Ops live in a block's ordered list and may not be reordered relative
// to one another without an analysis that proves it safe. Expressions, by
// contrast, are unordered and freely shared.
//
// Every Op carries the address of the machine instruction it came from and the
// identity of the pass that produced it. That provenance is a hard invariant
// checked by the verifier, and it is what later drives the `/* 0xADDR */`
// anchors in emitted C and the edge coverage gate.
#pragma once

#include <cstdint>
#include <string_view>

#include "xdec/il/expr.h"
#include "xdec/il/type.h"
#include "xdec/support/handle.h"

namespace xdec::il {

struct OpTag;
using OpId = Handle<OpTag>;

struct BlockTag;
using BlockId = Handle<BlockTag>;

struct RegTag;
/// Index into the target's register file.
using RegId = Handle<RegTag>;

/// Sentinel for an Op with no known source address. Only synthesised ops that
/// genuinely have no machine origin may use it, and the verifier rejects it at
/// Lifted maturity.
inline constexpr uint64_t kNoOpAddress = ~uint64_t{0};

enum OpFlags : uint16_t {
  OpFlagNone = 0,
  /// Ends a block. Exactly one, as the last op.
  OpFlagTerminator = 1u << 0,
  /// Defines exactly one value.
  OpFlagDefinesValue = 1u << 1,
  /// Must not be removed even if its result is unused.
  OpFlagSideEffects = 1u << 2,
  OpFlagReadsMemory = 1u << 3,
  OpFlagWritesMemory = 1u << 4,
  /// Reads or writes a machine register named by `payload`.
  OpFlagTouchesRegister = 1u << 5,
  /// Operand count is not fixed by the opcode.
  OpFlagVariadic = 1u << 6,
};

// name, text, flags
#define XDEC_OPS(X)                                                                            \
  /* Snapshots a register into a value. A read rather than an expression        */             \
  /* because the register's contents change over time.                          */             \
  X(ReadReg, "read", OpFlagDefinesValue | OpFlagTouchesRegister)                                \
  X(WriteReg, "write", OpFlagSideEffects | OpFlagTouchesRegister)                               \
  X(Load, "load", OpFlagDefinesValue | OpFlagReadsMemory)                                       \
  X(Store, "store", OpFlagSideEffects | OpFlagWritesMemory)                                     \
  /* An unconditional edge. targets[0] is the successor.                        */              \
  X(Branch, "br", OpFlagTerminator)                                                             \
  /* targets[0] is taken when the condition is true, targets[1] otherwise.      */              \
  X(CondBranch, "brc", OpFlagTerminator)                                                        \
  /* Computed branch. targets holds whatever has been resolved so far, and may  */              \
  /* legitimately be empty before the resolution phase runs.                    */              \
  X(IndirectBranch, "brind", OpFlagTerminator)                                                  \
  /* Not a terminator: control normally returns. A non-returning call is        */              \
  /* followed by Unreachable once a pass has established that.                  */             \
  X(Call, "call", OpFlagSideEffects | OpFlagReadsMemory | OpFlagWritesMemory)                    \
  X(Return, "ret", OpFlagTerminator)                                                            \
  /* A named operation whose effect is not modelled in the IL. This is the      */              \
  /* honest representation for SIMD, system registers and barriers: the         */             \
  /* operation is identified, its effect is opaque, and nothing is invented.    */             \
  X(Intrinsic, "intrinsic", OpFlagSideEffects | OpFlagVariadic)                                 \
  X(Nop, "nop", OpFlagNone)                                                                     \
  /* Control cannot reach here. Used after a proven non-returning call.         */              \
  X(Unreachable, "unreachable", OpFlagTerminator)                                               \
  /* An instruction that could not be decoded or lifted. A terminator, because  */              \
  /* everything after an unmodelled instruction in the same block is untrusted. */             \
  X(Unimplemented, "unimplemented", OpFlagTerminator)                                           \
  /* SSA merge. One operand per predecessor, in predecessor order.              */              \
  X(Phi, "phi", OpFlagDefinesValue | OpFlagVariadic)

enum class OpCode : uint8_t {
#define XDEC_OP_ENUM(name, text, flags) name,
  XDEC_OPS(XDEC_OP_ENUM)
#undef XDEC_OP_ENUM
      Count
};

struct OpCodeInfo {
  std::string_view text;
  uint16_t flags;
};

[[nodiscard]] const OpCodeInfo& info(OpCode code) noexcept;
[[nodiscard]] std::string_view toString(OpCode code) noexcept;
[[nodiscard]] bool parseOpCode(std::string_view text, OpCode& out) noexcept;

[[nodiscard]] inline bool isTerminator(OpCode code) noexcept {
  return (info(code).flags & OpFlagTerminator) != 0;
}
[[nodiscard]] inline bool definesValue(OpCode code) noexcept {
  return (info(code).flags & OpFlagDefinesValue) != 0;
}
[[nodiscard]] inline bool hasSideEffects(OpCode code) noexcept {
  return (info(code).flags & OpFlagSideEffects) != 0;
}
[[nodiscard]] inline bool touchesRegister(OpCode code) noexcept {
  return (info(code).flags & OpFlagTouchesRegister) != 0;
}
[[nodiscard]] inline bool isVariadic(OpCode code) noexcept {
  return (info(code).flags & OpFlagVariadic) != 0;
}

/// Identifies the pass that produced an Op. Zero is the lifter.
using PassId = uint16_t;
inline constexpr PassId kPassLifter = 0;

struct Op {
  OpCode code = OpCode::Nop;
  /// Result type for value-defining ops, and the accessed width for Load and
  /// Store. Void otherwise.
  Type type;
  /// Address of the machine instruction this came from.
  uint64_t va = kNoOpAddress;
  /// The pass that created it.
  PassId origin = kPassLifter;
  /// The value defined, when the opcode defines one.
  ValueId result;
  /// RegId for register access, interned name index for Intrinsic, unused
  /// otherwise.
  uint32_t payload = 0;
  /// Half-open range in the function's operand pool.
  uint32_t operandStart = 0;
  uint32_t operandCount = 0;
  /// Half-open range in the function's branch target pool.
  uint32_t targetStart = 0;
  uint32_t targetCount = 0;

  [[nodiscard]] bool isTerminator() const noexcept { return il::isTerminator(code); }
  [[nodiscard]] bool definesValue() const noexcept { return il::definesValue(code); }
  [[nodiscard]] RegId reg() const noexcept { return RegId{payload}; }
};

/// What a defined value is and where it came from.
struct ValueInfo {
  Type type;
  /// The op that defines it. Every value has exactly one definition.
  OpId definition;
  /// Block containing the definition, for dominance checks.
  BlockId block;
};

}  // namespace xdec::il
