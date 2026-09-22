// What one machine instruction computes, as a closed expression graph.
//
// The lifter already answers this: it turns an encoding into ReadReg/WriteReg
// ops over a pure expression pool, and that IL is xdec's authoritative statement
// of the instruction's meaning. A consumer outside the process -- a taint engine
// replaying a recorded trace, say -- cannot reach into an il::Function, and the
// alternative it is otherwise pushed towards is decoding the instruction word a
// second time. A second decoder is a second opinion: it drifts from the xspec
// rules, and the drift shows up as silent mismatches rather than build errors.
//
// So this is the IL, flattened into something copyable. It is deliberately not a
// classification -- no "this is a shift with amount 3" taxonomy that would need
// extending for every encoding and would have to re-recognise the rotate-and-mask
// idiom the bitfield rules lift `lsr` into. It is the operations themselves, with
// the constants the encoding folded in, which is strictly more information and
// requires no agreement about categories.
//
// A summary is a property of an encoding rather than of one execution, so it is
// computed once per compiled instruction and shared by every execution of it.
#pragma once

#include <cstdint>
#include <vector>

#include "xdec/il/expr.h"
#include "xdec/il/function.h"
#include "xdec/il/op.h"

namespace xdec::exec {

struct ExecInstruction;

/// Where a leaf value came from, packed into a SemanticNode's `immediate` when
/// its op is ExprOp::Value.
enum class LeafKind : uint8_t {
  /// A register the instruction read, named as the encoding names it. The index
  /// is the RegId.
  Register,
  /// The result of the instruction's Nth Load, counting in program order. The
  /// index is N, which is also the index into the execution's memory accesses
  /// once the reads among them are counted off in the same order.
  Load,
};

[[nodiscard]] constexpr uint64_t packLeaf(LeafKind kind, uint32_t index) noexcept {
  return (static_cast<uint64_t>(kind) << 32) | index;
}
[[nodiscard]] constexpr LeafKind leafKind(uint64_t immediate) noexcept {
  return static_cast<LeafKind>(immediate >> 32);
}
[[nodiscard]] constexpr uint32_t leafIndex(uint64_t immediate) noexcept {
  return static_cast<uint32_t>(immediate);
}

/// One node of the value graph.
struct SemanticNode {
  il::ExprOp op = il::ExprOp::Undef;
  /// Result width in bits; zero for the opaque flags type.
  uint16_t width = 0;
  uint8_t operandCount = 0;
  /// Indices into InstructionSemantics::nodes, always lower than this node's
  /// own index. The graph is emitted in topological order, so evaluating it is
  /// one forward pass with no recursion and no cycle check.
  uint16_t operands[il::kMaxExprOperands] = {};
  /// Const's value, Value's packed leaf, Extract's low bit, FlagDef's packing;
  /// zero for everything else, matching il::Expr.
  uint64_t immediate = 0;
};

/// Something the instruction changes.
struct SemanticEffect {
  enum class Kind : uint8_t { WriteReg, Store };

  Kind kind = Kind::WriteReg;
  /// The destination, for WriteReg. As the encoding names it: a 32-bit write
  /// reports `w0`, and the zero-extension of the parent that implies is the
  /// register file's business, not this summary's.
  il::RegId reg;
  /// The address written, for Store.
  uint16_t addressNode = 0;
  /// Bits written, for Store.
  uint16_t width = 0;
  uint16_t valueNode = 0;
};

/// Beyond this an instruction is not worth summarising, and the node indices
/// would not fit. Nothing real comes close: the widest bitfield rule lifts to
/// well under twenty nodes.
inline constexpr std::size_t kMaxSemanticNodes = 1024;

struct InstructionSemantics {
  std::vector<SemanticNode> nodes;
  /// In the order the lifter emitted them, which is the order they must be
  /// applied: an instruction may write a register it also reads.
  std::vector<SemanticEffect> effects;
  /// Whether every op the lifter produced is accounted for here. False when the
  /// instruction has an effect this cannot describe -- an intrinsic standing in
  /// for unmodelled SIMD, an encoding that did not lift -- and then the effects
  /// are a fragment rather than the whole story, so a consumer must not conclude
  /// anything from their absence.
  bool complete = false;

  [[nodiscard]] bool empty() const noexcept { return effects.empty(); }
};

/// Flattens the ops `instruction` owns into a summary.
///
/// `function` must be the one the instruction's OpIds belong to, at Lifted
/// maturity: the expressions are expected to bottom out in values this
/// instruction's own ReadReg and Load ops define. A reference to anything else
/// becomes an Undef leaf and clears `complete`, rather than being guessed at.
[[nodiscard]] InstructionSemantics summarize(const il::Function& function,
                                             const ExecInstruction& instruction);

}  // namespace xdec::exec
