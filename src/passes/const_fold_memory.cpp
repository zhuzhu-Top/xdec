// makeConstFoldMemoryPass: turns loads of never-changing memory into the
// constants they always yield (see the header for the soundness question and
// why the payoff is downstream rather than here).
#include "xdec/passes/const_fold_memory.h"

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "xdec/il/ceval.h"
#include "xdec/il/function.h"

#include "transform.h"

namespace xdec::passes {

namespace {

class ConstFoldMemory final : public pass::FunctionPass {
 public:
  ConstFoldMemory()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "const-fold-memory";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          info.requirements = {"ssa-construct"};
          // A folded load can be what makes the next load's address constant
          // (a pointer in read-only memory pointing into read-only memory), so
          // one sweep is not always enough. Each iteration strictly removes
          // loads, so the loop cannot run away.
          info.fixpoint = true;
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const ByteReader* image = context.image();
    if (image == nullptr) {
      // Unlike resolve-indirect — whose whole contract is resolution, so a
      // missing image there is a wiring error worth failing on — this pass
      // only ever removes work. Nothing to read means nothing to fold, and no
      // immutability facts means nothing may be assumed constant; either way
      // the conservative answer is the right one.
      return false;
    }

    il::Function& function = context.function();
    const MemoryFacts& facts = context.memoryFacts();
    ValueSubst subst;
    std::vector<std::pair<il::BlockId, il::OpId>> folded;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        uint64_t value = 0;
        if (!foldable(function, *image, facts, opId, value)) {
          continue;
        }
        const il::Op& op = function.op(opId);
        subst.set(op.result, function.constant(op.type, value));
        folded.emplace_back(blockId, opId);
      }
    }
    if (folded.empty()) {
      return false;
    }

    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const auto operands = function.operands(function.op(opId));
        if (operands.empty()) {
          continue;
        }
        std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
        bool touched = false;
        for (il::ExprId& operand : rewritten) {
          const il::ExprId next = subst.apply(function, operand);
          touched |= next != operand;
          operand = next;
        }
        if (touched) {
          function.setOperands(opId, rewritten);
        }
      }
    }
    // Every use was just rewritten to the constant, so the loads are dead and
    // removable here rather than left for the global DCE: a load left behind
    // would be read back as an unknown by the next pass that looks at it,
    // which is exactly the opacity this pass exists to remove.
    for (const auto& [blockId, opId] : folded) {
      function.removeOp(blockId, opId);
    }
    return true;
  }

 private:
  /// Whether `opId` is a load this pass may replace, and if so what it yields.
  [[nodiscard]] static bool foldable(const il::Function& function, const ByteReader& image,
                                     const MemoryFacts& facts, il::OpId opId, uint64_t& out) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Load || !op.result.valid()) {
      return false;
    }
    // Whole bytes, and narrow enough for a Const to hold exactly: the 128-bit
    // vector loads have no constant form to fold into, and a hypothetical
    // odd-width load has no unambiguous byte range to read.
    if (!op.type.isScalarInteger() || op.type.bits() % 8 != 0) {
      return false;
    }
    const auto operands = function.operands(op);
    if (operands.empty()) {
      return false;
    }
    il::ConcreteValue address;
    if (!il::tryEvalConst(function, operands[0], address) || address.hi != 0) {
      return false;
    }
    const unsigned width = op.type.bits() / 8;
    if (!facts.isImmutable(address.lo, width)) {
      return false;
    }
    std::array<std::byte, 8> bytes{};
    if (!image(address.lo, std::span<std::byte>(bytes).subspan(0, width))) {
      return false;  // immutable but unreadable: report nothing, invent nothing
    }
    // Little-endian assembly, as in resolve-indirect and ImageEval. Threading
    // the image's endianness to all three is one change, not three, and no
    // big-endian target exists to need it yet.
    uint64_t value = 0;
    for (unsigned index = 0; index < width; ++index) {
      value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    }
    out = value;
    return true;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeConstFoldMemoryPass() {
  return std::make_unique<ConstFoldMemory>();
}

}  // namespace xdec::passes
