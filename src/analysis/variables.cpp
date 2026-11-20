#include "xdec/analysis/variables.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_set>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/live_register_frame.h"
#include "xdec/analysis/stack_load_fold.h"
#include "xdec/analysis/typed_variables.h"
#include "xdec/types/database.h"

namespace xdec::analysis {

std::string CType::format() const {
  if (pointerDepth == 0) {
    if (isFloat) {
      if (width == 32) {
        return "float";
      }
      if (width == 64) {
        return "double";
      }
    }
    if (width == 1) {
      // A one-bit variable is a condition -- a phi merging two comparisons, or a
      // carry carried across blocks. `uint1_t` is not a type in any C, so the
      // only spelling that both compiles and says what the value is is `bool`.
      // Pointees are left alone below: no load is one bit wide, so a one-bit
      // pointee would be a bug elsewhere, and printing `bool*` would hide it.
      return "bool";
    }
    return std::format("{}int{}_t", isSigned ? "" : "u", width);
  }
  std::string base = pointeeWidth == 0
                         ? "void"
                         : std::format("{}int{}_t", isSigned ? "" : "u", pointeeWidth);
  for (uint32_t level = 0; level < pointerDepth; ++level) {
    base += '*';
  }
  return base;
}

namespace {

/// How wide a value is read, which is the only evidence a function body carries
/// about how wide the value really is.
///
/// A register is as wide as the machine makes it, but a variable is as wide as
/// the code reads. AArch64 passes a 32-bit argument in the low half of an
/// argument register and leaves the high half unspecified, so a function taking
/// `int32_t` reads its parameter through `w0` — a truncation in the IL — and
/// never touches the full register. Reading the full register is therefore the
/// distinguishing evidence, and it shows up as being consumed by anything other
/// than a truncation.
struct WidthUse {
  /// Read at the full width of whatever register or value holds it.
  bool fullWidth = false;
  /// The widest truncation it was read through, when only ever read through
  /// truncations.
  uint32_t truncWidth = 0;
  /// Read as a phi operand. Tracked apart from the two above because a phi over
  /// machine registers is as wide as the registers regardless of how much of
  /// them the program sets, so "this reaches a 64-bit phi" is a fact about SSA
  /// and not about the variable. What the phi is worth is decided from how the
  /// phi in turn is read.
  std::vector<il::ValueId> merges;
};

/// Iterative expression walk over every op operand, collecting the leaves
/// variable recovery cares about and the signedness evidence.
class Scan {
 public:
  explicit Scan(const il::Function& function) : function_(function) {}

  void run() {
    for (const il::BlockId blockId : function_.blockHandles()) {
      for (const il::OpId opId : function_.block(blockId).ops) {
        const il::Op& op = function_.op(opId);
        if (op.code == il::OpCode::Phi) {
          phis_.push_back(opId);
        }
        if (op.code == il::OpCode::Load || op.code == il::OpCode::Store) {
          memoryOps_.push_back(opId);
        }
        for (const il::ExprId operand : function_.operands(op)) {
          walk(operand, il::ExprId{}, op);
        }
      }
    }
  }

  std::unordered_set<uint32_t> seen_;
  std::vector<il::ExprId> entryLeaves_;      // every EntryReg leaf seen
  std::vector<il::OpId> memoryOps_;          // loads and stores
  std::vector<il::OpId> phis_;               // in block order
  std::vector<std::pair<il::ExprId, il::ExprId>> signedUses_;  // (leaf-ish, user)
  std::map<uint32_t, WidthUse> leafUses_;    // keyed by argument RegId index
  std::map<uint32_t, WidthUse> valueUses_;   // keyed by ValueId index

 private:
  void walk(il::ExprId root, il::ExprId parent, const il::Op& consumer) {
    if (!root.valid()) {
      return;
    }
    const il::Expr& expr = function_.expr(root);
    // Recorded before the visited check, and without recursing: expressions are
    // hash-consed, so one leaf is reached from every parent that reads it and all
    // of those parents are the evidence. Deduplicating interior nodes is still
    // safe, because a repeated interior node is the same parent.
    if (expr.op == il::ExprOp::EntryReg) {
      note(leafUses_[static_cast<uint32_t>(expr.immediate)], parent, consumer);
      entryLeaves_.push_back(root);
      return;
    }
    if (expr.op == il::ExprOp::Value) {
      note(valueUses_[static_cast<uint32_t>(expr.immediate)], parent, consumer);
      return;
    }
    if (!seen_.insert(root.index()).second) {
      return;
    }
    switch (expr.op) {
      case il::ExprOp::CmpLtS:
      case il::ExprOp::CmpLeS:
      case il::ExprOp::DivS:
      case il::ExprOp::RemS:
      case il::ExprOp::MulHiS:
      case il::ExprOp::SExt:
        // Signed operations are evidence about their operand leaves.
        for (unsigned index = 0; index < expr.operandCount; ++index) {
          signedUses_.emplace_back(expr.operand(index), root);
        }
        break;
      default:
        break;
    }
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      walk(expr.operand(index), root, consumer);
    }
  }

  void note(WidthUse& use, il::ExprId parent, const il::Op& consumer) {
    if (parent.valid()) {
      const il::Expr& parentExpr = function_.expr(parent);
      if (parentExpr.op == il::ExprOp::Trunc) {
        use.truncWidth = std::max(use.truncWidth, parentExpr.type.bits());
      } else {
        use.fullWidth = true;
      }
      return;
    }
    // Handed straight to an operation, with no expression in between.
    switch (consumer.code) {
      case il::OpCode::Phi:
        // One entry per merge, however many of that merge's operands are this
        // same value. A dispatcher's phi has an operand per case and most of
        // them read the same register, so recording one entry each would make
        // every later walk redo that work for no fact it did not already have.
        if (std::find(use.merges.begin(), use.merges.end(), consumer.result) ==
            use.merges.end()) {
          use.merges.push_back(consumer.result);
        }
        return;
      case il::OpCode::Return:
        // Not a demand: how wide a return is read is the caller's business, and
        // the very thing the return type is being recovered to answer.
        return;
      default:
        use.fullWidth = true;
        return;
    }
  }

  const il::Function& function_;
};

/// The width a value is read at, given how it is used.
///
/// Unset when nothing demands a particular width, which is different from
/// demanding the full one: a value that is only merged and returned has no
/// reader that pins it down, and saying "64 bits" there would be inventing
/// evidence rather than reporting it.
///
/// `walked` is what keeps this affordable. The answer is a maximum over every
/// merge reachable from the value, so a merge already visited can only re-derive
/// the number it already contributed — but visiting it again is not free, and on
/// a flattened function it is ruinous: a resolved dispatcher gives its merge
/// point one phi per live register with an operand per case, the same value
/// appears on hundreds of those operands, and so the same phi lands in `merges`
/// hundreds of times. Re-walking it each time is that fan-out raised to the
/// depth of the merge graph. Visiting once makes the walk linear in the graph.
[[nodiscard]] std::optional<uint32_t> readWidth(
    const std::map<uint32_t, WidthUse>& valueUses, const WidthUse& use,
    uint32_t declared, std::unordered_set<uint32_t>& walked) {
  if (use.fullWidth) {
    return declared;
  }
  std::optional<uint32_t> widest;
  if (use.truncWidth != 0) {
    widest = use.truncWidth;
  }
  // A value that only survives into a merge is as wide as that merge is read.
  // This is the shape of a parameter driving a state machine: the argument
  // register is never touched directly, only merged with the next state at the
  // loop header, and every reader of that merge truncates.
  for (const il::ValueId merge : use.merges) {
    if (!walked.insert(merge.index()).second) {
      continue;
    }
    const auto found = valueUses.find(merge.index());
    if (found == valueUses.end()) {
      continue;
    }
    if (const std::optional<uint32_t> through =
            readWidth(valueUses, found->second, declared, walked)) {
      widest = std::max(widest.value_or(0), *through);
    }
  }
  return widest;
}

/// The same, starting a fresh walk.
[[nodiscard]] std::optional<uint32_t> readWidth(
    const std::map<uint32_t, WidthUse>& valueUses, const WidthUse& use,
    uint32_t declared) {
  std::unordered_set<uint32_t> walked;
  return readWidth(valueUses, use, declared, walked);
}

/// Peels address arithmetic and casts to the leaf an address is based on,
/// when there is one: entry(arg), a value, or a plain constant.
il::ExprId addressBase(const il::Function& function, il::ExprId address) {
  il::ExprId cur = address;
  for (unsigned depth = 0; depth < 16 && cur.valid(); ++depth) {
    const il::Expr& expr = function.expr(cur);
    switch (expr.op) {
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt:
      case il::ExprOp::Trunc:
        cur = expr.operand(0);
        continue;
      case il::ExprOp::Add:
      case il::ExprOp::Sub: {
        const il::Expr& rhs = function.expr(expr.operand(1));
        if (rhs.op == il::ExprOp::Const) {
          cur = expr.operand(0);
          continue;
        }
        const il::Expr& lhs = function.expr(expr.operand(0));
        if (expr.op == il::ExprOp::Add && lhs.op == il::ExprOp::Const) {
          cur = expr.operand(1);
          continue;
        }
        return cur;
      }
      default:
        return cur;
    }
  }
  return cur;
}

[[nodiscard]] uint32_t significantBits(uint64_t value) {
  uint32_t bits = 0;
  while (value != 0) {
    ++bits;
    value >>= 1;
  }
  return bits;
}

/// A stack slot's dispatcher-state signal: how many distinct small integers
/// (an enum-sized range, not data) it is ever seen being assigned. A
/// flattening obfuscator threads its state through a slot like this one --
/// stored fresh before nearly every computed branch, each site a different
/// literal -- so a slot with many such literals is that thread made visible,
/// even though nothing here follows the branches to prove it. Constants are
/// collected through one level of `Select` so the common `cond ? a : b` a
/// two-way dispatch leaves behind counts as two candidate states rather than
/// none. Values above a small-enum range are ignored: a hash, address, or
/// counter stored to the same slot elsewhere must not read as a state.
constexpr uint64_t kStateConstantMax = 0xff;
constexpr std::size_t kStateConstantThreshold = 4;

void collectSmallConstants(const il::Function& function, il::ExprId id,
                           std::set<uint64_t>& out, unsigned depth = 0) {
  if (!id.valid() || depth > 3) {
    return;
  }
  const il::Expr& expr = function.expr(id);
  if (expr.op == il::ExprOp::Const) {
    if (expr.immediate <= kStateConstantMax) {
      out.insert(expr.immediate);
    }
    return;
  }
  if (expr.op == il::ExprOp::Select) {
    collectSmallConstants(function, expr.operand(1), out, depth + 1);
    collectSmallConstants(function, expr.operand(2), out, depth + 1);
  }
}

/// How many bits of a value the function itself sets.
///
/// SSA over machine registers gives every value the width of the register that
/// held it, so a 32-bit computation that survives a branch becomes a 64-bit phi
/// of zero-extensions, and a 32-bit result handed back becomes a 64-bit `ret`.
/// Reading those declared widths back out as types is what makes every function
/// in a stripped binary appear to work in 64 bits. The operands say something
/// narrower and true: an extension from `w` sets `w` bits, a constant sets as
/// many as it needs, an argument register sets however wide the argument was
/// found to be, and a cycle back to the value being asked about sets nothing new.
class MergeWidth {
 public:
  MergeWidth(const il::Function& function, std::map<uint32_t, uint32_t> argumentWidths)
      : function_(function), argumentWidths_(std::move(argumentWidths)) {}

  /// Bits set, and whether the widening was a sign extension — the one place a
  /// value's signedness is directly observable rather than guessed.
  struct Result {
    uint32_t width = 0;
    bool isSigned = false;
    /// Whether anything here says how wide the value is. A literal does not: zero
    /// fits in every type, so `return 0` is evidence about no width at all, and
    /// treating its zero bits as a measurement would report a byte where a
    /// neighbouring path plainly returns a word.
    bool strong = false;
  };

  [[nodiscard]] Result of(il::ExprId root) {
    active_.clear();
    Result out;
    measure(root, 0, out);
    return out;
  }

 private:
  void measure(il::ExprId id, unsigned depth, Result& out) {
    if (!id.valid() || depth > 24) {
      return;
    }
    const il::Expr& expr = function_.expr(id);
    switch (expr.op) {
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt: {
        out.isSigned = out.isSigned || expr.op == il::ExprOp::SExt;
        const il::ExprId inner = expr.operand(0);
        Result narrower;
        measure(inner, depth + 1, narrower);
        const uint32_t declared = function_.expr(inner).type.bits();
        note(out, std::min(declared, narrower.strong ? narrower.width : declared), true);
        return;
      }
      case il::ExprOp::Const:
        note(out, std::min(expr.type.bits(), significantBits(expr.immediate)), false);
        return;
      case il::ExprOp::EntryReg: {
        const auto found = argumentWidths_.find(static_cast<uint32_t>(expr.immediate));
        note(out, found == argumentWidths_.end() ? expr.type.bits() : found->second, true);
        return;
      }
      case il::ExprOp::Value: {
        const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
        if (!function_.hasValue(value)) {
          note(out, expr.type.bits(), true);
          return;
        }
        const il::OpId definition = function_.value(value).definition;
        if (!definition.valid() ||
            function_.op(definition).code != il::OpCode::Phi) {
          note(out, expr.type.bits(), true);
          return;
        }
        if (!active_.insert(value.index()).second) {
          return;  // a loop-carried value adds nothing its operands do not
        }
        for (const il::ExprId operand : function_.operands(function_.op(definition))) {
          measure(operand, depth + 1, out);
        }
        return;
      }
      default:
        note(out, expr.type.bits(), true);
        return;
    }
  }

  static void note(Result& out, uint32_t width, bool strong) {
    out.width = std::max(out.width, width);
    out.strong = out.strong || strong;
  }

  const il::Function& function_;
  std::map<uint32_t, uint32_t> argumentWidths_;
  std::unordered_set<uint32_t> active_;
};

/// The type of the value the function hands back, or nothing when no `ret`
/// carries one.
[[nodiscard]] std::optional<CType> returnedType(const il::Function& function,
                                                MergeWidth& widths) {
  bool found = false;
  bool measured = false;
  bool isSigned = false;
  uint32_t evidence = 0;
  uint32_t declared = 0;
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Return) {
        continue;
      }
      const auto operands = function.operands(op);
      if (operands.empty() ||
          function.expr(operands[0]).op == il::ExprOp::Undef ||
          !function.expr(operands[0]).type.isScalarInteger()) {
        continue;
      }
      found = true;
      declared = std::max(declared, function.expr(operands[0]).type.bits());
      const MergeWidth::Result result = widths.of(operands[0]);
      if (!result.strong) {
        // A path returning only a literal fits whatever the other paths need,
        // so it is passed over rather than counted as a full-width return.
        continue;
      }
      measured = true;
      isSigned = isSigned || result.isSigned;
      // Disagreeing returns are widened together rather than picking one: the
      // narrow path's value still has to fit in whatever the prototype says.
      evidence = std::max(evidence, result.width);
    }
  }
  if (!found) {
    return std::nullopt;
  }
  CType out;
  out.width = measured ? std::max(evidence, 8u) : declared;
  out.isSigned = isSigned;
  return out;
}

/// Sparse SSA Phi copy coalescing: merges non-interfering `phi_dst = phi(...,
/// val(phi_src), ...)` chains that share an identical CType into a single C
/// variable so Out-of-SSA emission elides pure relay copies (`t2 = t1;`).
void coalescePhis(const il::Function& function, std::span<const il::OpId> phis,
                  const std::vector<uint8_t>& pinned, std::vector<Variable>& temps,
                  std::map<uint32_t, uint32_t>& tempByValue,
                  std::vector<std::vector<il::ValueId>>& tempValues) {
  const std::size_t n = phis.size();
  if (n < 2) {
    tempValues.resize(temps.size());
    for (std::size_t i = 0; i < temps.size(); ++i) {
      tempValues[i].push_back(temps[i].value);
    }
    return;
  }

  std::vector<int32_t> phiIndexByVal(function.valueCount(), -1);
  std::vector<il::BlockId> defBlock(n);
  for (std::size_t i = 0; i < n; ++i) {
    const il::Op& op = function.op(phis[i]);
    if (op.result.valid()) {
      phiIndexByVal[op.result.index()] = static_cast<int32_t>(i);
      defBlock[i] = function.value(op.result).block;
    }
  }

  // Collect non-Phi body uses and Phi incoming-edge uses for every Phi value.
  std::vector<std::vector<il::BlockId>> bodyUses(n);
  std::vector<std::vector<std::pair<il::BlockId, il::BlockId>>> phiEdgeUses(n);
  std::vector<uint32_t> exprSeen(function.exprCount(), 0);
  uint32_t exprEpoch = 0;
  std::vector<il::ExprId> exprWork;

  const auto collectOpPhiLeaves = [&](il::ExprId root, auto&& onPhiLeaf) {
    if (++exprEpoch == 0) {
      std::fill(exprSeen.begin(), exprSeen.end(), 0);
      exprEpoch = 1;
    }
    exprWork.clear();
    exprWork.push_back(root);
    while (!exprWork.empty()) {
      const il::ExprId id = exprWork.back();
      exprWork.pop_back();
      if (id.index() < exprSeen.size()) {
        if (exprSeen[id.index()] == exprEpoch) {
          continue;
        }
        exprSeen[id.index()] = exprEpoch;
      }
      const il::Expr& expr = function.expr(id);
      if (expr.op == il::ExprOp::Value) {
        const auto val = il::ValueId{static_cast<uint32_t>(expr.immediate)};
        if (val.valid() && val.index() < phiIndexByVal.size() &&
            phiIndexByVal[val.index()] >= 0) {
          onPhiLeaf(static_cast<std::size_t>(phiIndexByVal[val.index()]));
        }
        continue;
      }
      for (unsigned k = 0; k < expr.operandCount; ++k) {
        exprWork.push_back(expr.operands[k]);
      }
    }
  };

  for (const il::BlockId blockId : function.blockHandles()) {
    const auto& preds = function.block(blockId).predecessors;
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code == il::OpCode::Phi) {
        const auto operands = function.operands(op);
        for (std::size_t slot = 0; slot < operands.size() && slot < preds.size(); ++slot) {
          collectOpPhiLeaves(operands[slot], [&](std::size_t leafIdx) {
            phiEdgeUses[leafIdx].emplace_back(preds[slot], blockId);
          });
        }
      } else {
        for (const il::ExprId operand : function.operands(op)) {
          collectOpPhiLeaves(operand, [&](std::size_t leafIdx) {
            bodyUses[leafIdx].push_back(blockId);
          });
        }
      }
    }
  }

  // Sparse SSA liveness per Phi (bounded backward walk from uses to defBlock).
  const Dominators dom = Dominators::compute(function);
  std::vector<std::vector<uint32_t>> liveInBody(n);
  std::vector<std::vector<uint32_t>> liveOutOf(n);
  std::vector<uint32_t> blockSeen(function.blockCount(), 0);
  uint32_t blockEpoch = 0;
  std::vector<il::BlockId> cfgWork;

  for (std::size_t i = 0; i < n; ++i) {
    const il::BlockId defB = defBlock[i];
    if (!defB.valid() || !dom.reachable(defB)) {
      continue;
    }
    if (++blockEpoch == 0) {
      std::fill(blockSeen.begin(), blockSeen.end(), 0);
      blockEpoch = 1;
    }
    cfgWork.clear();
    const auto markLiveIn = [&](il::BlockId b) {
      if (!b.valid() || b == defB || !dom.dominates(defB, b)) {
        return;
      }
      if (blockSeen[b.index()] != blockEpoch) {
        blockSeen[b.index()] = blockEpoch;
        liveInBody[i].push_back(b.index());
        cfgWork.push_back(b);
      }
    };
    for (const il::BlockId useB : bodyUses[i]) {
      if (useB != defB) {
        markLiveIn(useB);
      }
    }
    for (const auto& [predB, phiBlock] : phiEdgeUses[i]) {
      (void)phiBlock;
      if (predB.valid() && dom.dominates(defB, predB)) {
        liveOutOf[i].push_back(predB.index());
        markLiveIn(predB);
      }
    }
    while (!cfgWork.empty()) {
      const il::BlockId cur = cfgWork.back();
      cfgWork.pop_back();
      for (const il::BlockId pred : function.block(cur).predecessors) {
        if (pred.valid() && dom.dominates(defB, pred)) {
          liveOutOf[i].push_back(pred.index());
          markLiveIn(pred);
        }
      }
    }
    std::sort(liveInBody[i].begin(), liveInBody[i].end());
    liveInBody[i].erase(std::unique(liveInBody[i].begin(), liveInBody[i].end()),
                        liveInBody[i].end());
    std::sort(liveOutOf[i].begin(), liveOutOf[i].end());
    liveOutOf[i].erase(std::unique(liveOutOf[i].begin(), liveOutOf[i].end()),
                       liveOutOf[i].end());
  }

  // Union-Find over Phi classes.
  std::vector<std::size_t> parent(n);
  std::vector<std::vector<std::size_t>> members(n);
  std::vector<std::vector<uint32_t>> classDefs(n);
  for (std::size_t i = 0; i < n; ++i) {
    parent[i] = i;
    members[i].push_back(i);
    if (defBlock[i].valid()) {
      classDefs[i].push_back(defBlock[i].index());
    }
  }
  auto findRep = [&](std::size_t x) -> std::size_t {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  const auto sameCType = [](const CType& a, const CType& b) {
    return a.width == b.width && a.pointerDepth == b.pointerDepth &&
           a.pointeeWidth == b.pointeeWidth && a.isSigned == b.isSigned &&
           a.isFloat == b.isFloat;
  };

  const auto sortedOverlaps = [](const std::vector<uint32_t>& a,
                                 const std::vector<uint32_t>& b) {
    std::size_t ia = 0;
    std::size_t ib = 0;
    while (ia < a.size() && ib < b.size()) {
      if (a[ia] == b[ib]) {
        return true;
      }
      if (a[ia] < b[ib]) {
        ++ia;
      } else {
        ++ib;
      }
    }
    return false;
  };

  // Avoid merging if any sibling Phi in `phiB`'s block reads `classA` on an
  // edge where `phiB` writes a value outside `classA U classB` (keeps printEdge
  // free of self-induced snapshots).
  const auto hasEdgeCopyConflict = [&](std::size_t classA, std::size_t classB) {
    for (const std::size_t m : members[classB]) {
      const il::BlockId blk = defBlock[m];
      if (!blk.valid()) {
        continue;
      }
      const auto& preds = function.block(blk).predecessors;
      const auto mOperands = function.operands(function.op(phis[m]));
      for (const il::OpId otherOpId : function.block(blk).ops) {
        const il::Op& otherOp = function.op(otherOpId);
        if (otherOp.code != il::OpCode::Phi) {
          break;
        }
        if (otherOpId == phis[m]) {
          continue;
        }
        const auto otherOperands = function.operands(otherOp);
        for (std::size_t slot = 0;
             slot < otherOperands.size() && slot < mOperands.size() && slot < preds.size();
             ++slot) {
          bool readsA = false;
          collectOpPhiLeaves(otherOperands[slot], [&](std::size_t leafIdx) {
            const std::size_t rep = findRep(leafIdx);
            if (rep == classA || rep == classB) {
              readsA = true;
            }
          });
          if (!readsA) {
            continue;
          }
          const il::Expr& mSrc = function.expr(mOperands[slot]);
          bool writesSame = false;
          if (mSrc.op == il::ExprOp::Value) {
            const auto v = il::ValueId{static_cast<uint32_t>(mSrc.immediate)};
            if (v.valid() && v.index() < phiIndexByVal.size() &&
                phiIndexByVal[v.index()] >= 0) {
              const std::size_t rep = findRep(static_cast<std::size_t>(phiIndexByVal[v.index()]));
              writesSame = (rep == classA || rep == classB);
            }
          }
          if (!writesSame) {
            return true;
          }
        }
      }
    }
    return false;
  };

  const auto unionVec = [](std::vector<uint32_t>& dst, const std::vector<uint32_t>& src) {
    std::vector<uint32_t> merged;
    merged.reserve(dst.size() + src.size());
    std::merge(dst.begin(), dst.end(), src.begin(), src.end(), std::back_inserter(merged));
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    dst = std::move(merged);
  };

  // Cap live-set sizes for pathological functions so merging stays fast.
  constexpr std::size_t kMaxClassMergeMembers = 64;

  for (std::size_t b = 0; b < n; ++b) {
    if (pinned[b] != 0) {
      continue;
    }
    for (const il::ExprId operand : function.operands(function.op(phis[b]))) {
      const il::Expr& expr = function.expr(operand);
      if (expr.op != il::ExprOp::Value) {
        continue;
      }
      const auto srcVal = il::ValueId{static_cast<uint32_t>(expr.immediate)};
      if (!srcVal.valid() || srcVal.index() >= phiIndexByVal.size() ||
          phiIndexByVal[srcVal.index()] < 0) {
        continue;
      }
      const std::size_t a = static_cast<std::size_t>(phiIndexByVal[srcVal.index()]);
      if (pinned[a] != 0) {
        continue;
      }
      std::size_t ra = findRep(a);
      std::size_t rb = findRep(b);
      if (ra == rb) {
        continue;
      }
      if (members[ra].size() + members[rb].size() > kMaxClassMergeMembers) {
        continue;
      }
      if (!sameCType(temps[a].type, temps[b].type)) {
        continue;
      }
      if (sortedOverlaps(classDefs[ra], classDefs[rb]) ||
          sortedOverlaps(classDefs[ra], liveInBody[rb]) ||
          sortedOverlaps(classDefs[ra], liveOutOf[rb]) ||
          sortedOverlaps(classDefs[rb], liveInBody[ra]) ||
          sortedOverlaps(classDefs[rb], liveOutOf[ra])) {
        continue;
      }
      if (hasEdgeCopyConflict(ra, rb) || hasEdgeCopyConflict(rb, ra)) {
        continue;
      }
      // Keep the earlier-ordered Phi representative.
      if (ra > rb) {
        std::swap(ra, rb);
      }
      parent[rb] = ra;
      unionVec(classDefs[ra], classDefs[rb]);
      unionVec(liveInBody[ra], liveInBody[rb]);
      unionVec(liveOutOf[ra], liveOutOf[rb]);
      members[ra].insert(members[ra].end(), members[rb].begin(), members[rb].end());
      classDefs[rb].clear();
      liveInBody[rb].clear();
      liveOutOf[rb].clear();
      members[rb].clear();
    }
  }

  // Rebuild `temps`, `tempByValue`, and `tempValues` with contiguous t0..tK names.
  std::vector<Variable> compactTemps;
  compactTemps.reserve(temps.size());
  tempValues.clear();
  tempByValue.clear();
  uint32_t ordinal = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (findRep(i) != i) {
      continue;
    }
    Variable repVar = temps[i];
    if (pinned[i] == 0) {
      repVar.name = std::format("t{}", ordinal++);
    }
    const uint32_t newIdx = static_cast<uint32_t>(compactTemps.size());
    std::vector<il::ValueId> groupVals;
    groupVals.reserve(members[i].size());
    for (const std::size_t m : members[i]) {
      const il::ValueId mv = temps[m].value;
      tempByValue[mv.index()] = newIdx;
      groupVals.push_back(mv);
    }
    compactTemps.push_back(std::move(repVar));
    tempValues.push_back(std::move(groupVals));
  }
  temps = std::move(compactTemps);
}

}  // namespace

VariableTable VariableTable::recover(const il::Function& function,
                                     const StackFrame& frame,
                                     const AbiSpec* explicitAbi) {
  const AbiSpec defaultAbi =
      explicitAbi == nullptr ? AbiSpec::defaultFor(function) : AbiSpec{};
  const AbiSpec& activeAbi = explicitAbi != nullptr ? *explicitAbi : defaultAbi;
  VariableTable table;
  Scan scan(function);
  scan.run();

  // Arguments: entry leaves of argument registers. One variable per register,
  // as wide as the body reads it rather than as wide as the register is.
  for (const il::ExprId leaf : scan.entryLeaves_) {
    const il::Expr& expr = function.expr(leaf);
    const il::RegId root{static_cast<uint32_t>(expr.immediate)};
    const int index = activeAbi.argIndex(root);
    if (index < 0 || table.argByRoot_.contains(root.index())) {
      continue;
    }
    Variable var;
    var.kind = VarKind::Argument;
    var.name = std::format("a{}", index);
    var.argRoot = root;
    const uint32_t declared = function.registers()[root].type().bits();
    var.type.width = declared;
    if (const auto found = scan.leafUses_.find(root.index());
        found != scan.leafUses_.end()) {
      if (const std::optional<uint32_t> read =
              readWidth(scan.valueUses_, found->second, declared)) {
        var.type.width = *read;
      }
    }
    // Signedness is left unsigned here and promoted below only where a signed
    // operation says so. Guessing it from width used to be harmless because
    // almost everything came out 64 bits wide; now that widths are recovered the
    // guess would apply to most variables in most functions, and a wrong `int`
    // reads as a fact rather than as the absence of one.
    table.argByRoot_[root.index()] = static_cast<uint32_t>(table.args_.size());
    table.args_.push_back(std::move(var));
  }

  // The return type depends on the argument widths, because a value handed back
  // on one path can be an argument register handed straight through.
  std::map<uint32_t, uint32_t> argumentWidths;
  for (const auto& [rootIndex, argIndex] : table.argByRoot_) {
    argumentWidths[rootIndex] = table.args_[argIndex].type.width;
  }
  MergeWidth widths(function, std::move(argumentWidths));
  table.returnType_ = returnedType(function, widths);

  // Locals: one per observed stack-slot delta, widest access wins.
  std::map<int64_t, uint32_t> slotWidths;
  std::map<int64_t, std::set<uint64_t>> slotConstants;
  for (const il::OpId opId : scan.memoryOps_) {
    const il::Op& op = function.op(opId);
    const auto operands = function.operands(op);
    const AddressInfo info = frame.classify(operands[0]);
    if (info.kind != AddressKind::StackSlot) {
      continue;
    }
    uint32_t& width = slotWidths[info.delta];
    width = std::max(width, op.type.bits());
    if (op.code == il::OpCode::Store) {
      collectSmallConstants(function, operands[1], slotConstants[info.delta]);
    }
  }
  // Negative stack-slot addresses handed directly to a Call or Intrinsic as a
  // buffer pointer (`&var_b8`) also need a local variable even when the caller
  // itself never loaded or stored that offset directly.
  for (const il::BlockId blockId : function.blockHandles()) {
    for (const il::OpId opId : function.block(blockId).ops) {
      const il::Op& op = function.op(opId);
      if (op.code != il::OpCode::Call && op.code != il::OpCode::Intrinsic) {
        continue;
      }
      const auto operands = function.operands(op);
      const std::size_t startIdx = op.code == il::OpCode::Call ? 1 : 0;
      for (std::size_t i = startIdx; i < operands.size(); ++i) {
        const AddressInfo info = frame.classify(operands[i]);
        if (info.kind == AddressKind::StackSlot && info.delta < 0) {
          slotWidths.try_emplace(info.delta, 64u);
        }
      }
    }
  }
  // At most one slot per function is promoted to `state`: a second slot that
  // also happens to collect several small literals is far more likely to be
  // an ordinary counter or flags word than a second dispatcher, and handing
  // out the same name twice would be a worse reading than not promoting it.
  // Among the slots that qualify, the one with the most distinct literals is
  // kept rather than the first one found by ascending delta: a flattening
  // obfuscator's real dispatcher slot collects roughly one value per state the
  // function has, so the widest spread of small integers is the strongest
  // signal available without following the branches themselves. (A read
  // requirement was tried here and dropped: the reference dispatcher this
  // heuristic is tuned against, samples/manifest.json's sample_jni_onload,
  // keeps its state value live in a register/phi between the spill and its
  // use in the table address, so the slot that carries it is store-only --
  // excluding store-only slots throws away exactly the case this exists for.)
  std::optional<int64_t> stateDelta;
  std::size_t stateDeltaCount = 0;
  for (const auto& [delta, constants] : slotConstants) {
    if (constants.size() < kStateConstantThreshold) {
      continue;
    }
    if (constants.size() > stateDeltaCount) {
      stateDeltaCount = constants.size();
      stateDelta = delta;
    }
  }
  for (const auto& [delta, width] : slotWidths) {
    Variable var;
    var.kind = VarKind::Local;
    if (stateDelta.has_value() && delta == *stateDelta) {
      var.name = "state";
    } else {
      var.name = delta <= 0 ? std::format("var_{:x}", -delta)
                            : std::format("starg_{:x}", delta);
    }
    var.stackDelta = delta;
    var.type.width = std::max(width, 8u);
    table.localByDelta_[delta] = static_cast<uint32_t>(table.locals_.size());
    table.locals_.push_back(std::move(var));
  }

  // Temps: one per phi, in block order. A scatter dispatcher's own state lives
  // in registers rather than the slot promoted above, and naming those phis
  // `state` too was tried and dropped -- see eval/FINDINGS.md's 2026-08-13
  // note for the measurements: nothing in this corpus keeps a scatter region's
  // state in one register a reader could follow, so every rule that named
  // something named the wrong thing.
  uint32_t ordinal = 0;
  for (const il::OpId opId : scan.phis_) {
    const il::Op& op = function.op(opId);
    if (!op.result.valid()) {
      continue;
    }
    Variable var;
    var.kind = VarKind::Temp;
    var.name = std::format("t{}", ordinal++);
    var.value = op.result;
    const il::Type valType = function.value(op.result).type;
    var.type.width = valType.bits();
    var.type.isFloat = valType.isFloat();
    table.tempByValue_[op.result.index()] =
        static_cast<uint32_t>(table.temps_.size());
    table.temps_.push_back(std::move(var));
  }

  // A stack slot stack-load-fold proved is read only to be used as another
  // access's address (see analysis::findFoldableStackLoads) points somewhere
  // just as surely as an argument or temp does; computed once, up front, so
  // the loop below can treat it as one more base kind alongside those two.
  const std::unordered_map<uint32_t, FoldableStackLoad> foldableLoads =
      findFoldableStackLoads(function, frame, {});

  // Pointer refinement: an argument, temp, or spilled-pointer slot used as
  // the base of a memory address points somewhere; the widest access
  // through it types the pointee.
  for (const il::OpId opId : scan.memoryOps_) {
    const il::Op& op = function.op(opId);
    const auto operands = function.operands(op);
    const il::ExprId base = addressBase(function, operands[0]);
    if (!base.valid()) {
      continue;
    }
    const il::Expr& baseExpr = function.expr(base);
    Variable* target = nullptr;
    if (baseExpr.op == il::ExprOp::EntryReg) {
      const il::RegId root{static_cast<uint32_t>(baseExpr.immediate)};
      if (const auto found = table.argByRoot_.find(root.index());
          found != table.argByRoot_.end()) {
        target = &table.args_[found->second];
      }
    } else if (baseExpr.op == il::ExprOp::Value) {
      const il::ValueId value{static_cast<uint32_t>(baseExpr.immediate)};
      if (const auto found = table.tempByValue_.find(value.index());
          found != table.tempByValue_.end()) {
        target = &table.temps_[found->second];
      } else if (function.hasValue(value)) {
        // A pointer a stack slot holds -- an out-parameter's address stashed
        // in a spill, say -- points the slot at whatever this access reads
        // through it, the same evidence an argument or temp base is
        // promoted from just above. Restricted to a slot stack-load-fold
        // already proved is read *only* for this (see FoldableStackLoad's
        // own note), so a slot also read for its own value elsewhere is
        // left a plain integer rather than mistyped.
        const il::OpId definition = function.value(value).definition;
        if (const auto load = definition.valid() ? foldableLoads.find(definition.index())
                                                  : foldableLoads.end();
            load != foldableLoads.end() && load->second.usedAsAddress) {
          if (const auto localFound = table.localByDelta_.find(load->second.delta);
              localFound != table.localByDelta_.end()) {
            target = &table.locals_[localFound->second];
          }
        }
      }
    }
    if (target == nullptr) {
      continue;
    }
    if (target->type.pointerDepth == 0) {
      target->type.pointerDepth = 1;
      target->type.pointeeWidth = op.type.bits();
      target->type.isSigned = false;
    } else if (op.type.bits() != target->type.pointeeWidth) {
      // Conflicting access widths: the honest answer is an opaque pointer.
      target->type.pointeeWidth = 0;
    }
  }

  // A value that flows into a pointer is a pointer. This is what a loop walking
  // an array looks like after SSA: the parameter is never dereferenced itself,
  // only the phi that starts at it and advances by the element size, so without
  // following the phi the parameter reads as an integer that a pointer is
  // mysteriously derived from. Applied to a fixpoint because one phi can feed
  // another, and only in the pointer direction — a pointer merged with an
  // integer is still an address.
  bool changed = true;
  for (unsigned pass = 0; pass < 8 && changed; ++pass) {
    changed = false;
    for (const il::OpId opId : scan.phis_) {
      const il::Op& op = function.op(opId);
      if (!op.result.valid()) {
        continue;
      }
      const auto found = table.tempByValue_.find(op.result.index());
      if (found == table.tempByValue_.end()) {
        continue;
      }
      const CType merged = table.temps_[found->second].type;
      if (merged.pointerDepth == 0) {
        continue;
      }
      for (const il::ExprId operand : function.operands(op)) {
        const il::Expr& expr = function.expr(operand);
        Variable* target = nullptr;
        if (expr.op == il::ExprOp::EntryReg) {
          const il::RegId root{static_cast<uint32_t>(expr.immediate)};
          if (const auto arg = table.argByRoot_.find(root.index());
              arg != table.argByRoot_.end()) {
            target = &table.args_[arg->second];
          }
        } else if (expr.op == il::ExprOp::Value) {
          const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
          if (const auto temp = table.tempByValue_.find(value.index());
              temp != table.tempByValue_.end()) {
            target = &table.temps_[temp->second];
          }
        }
        if (target == nullptr || target->type.pointerDepth != 0) {
          continue;
        }
        target->type.pointerDepth = merged.pointerDepth;
        target->type.pointeeWidth = merged.pointeeWidth;
        target->type.isSigned = false;
        changed = true;
      }
    }
  }

  // Signedness refinement: any signed operation over a variable's leaf
  // promotes it. Only the leaf itself counts — a signed use of a derived
  // value says nothing about the source.
  for (const auto& [leafId, user] : scan.signedUses_) {
    (void)user;
    const il::Expr& leaf = function.expr(leafId);
    if (leaf.op == il::ExprOp::EntryReg) {
      const il::RegId root{static_cast<uint32_t>(leaf.immediate)};
      if (const auto found = table.argByRoot_.find(root.index());
          found != table.argByRoot_.end()) {
        table.args_[found->second].type.isSigned = true;
      }
    } else if (leaf.op == il::ExprOp::Value) {
      const il::ValueId value{static_cast<uint32_t>(leaf.immediate)};
      if (const auto found = table.tempByValue_.find(value.index());
          found != table.tempByValue_.end()) {
        table.temps_[found->second].type.isSigned = true;
      }
    }
  }

  // A flattening dispatcher's own argument-register relay (see
  // analysis::LiveRegisterFrame): the two phis a register gets, one merging
  // every handler's exit value and one merging that against the function's
  // entry, read as two arbitrary temporaries otherwise -- `x{N}_live` (what
  // this iteration's handler sees) and `x{N}_exit` (what it leaves there)
  // says which register and which of the two phis this is, the same way
  // `state` already says which stack slot drives the dispatch. Found by a
  // fresh scan rather than threaded in from structuring: this runs before
  // structureFunction does, and the shape is cheap enough to look for again
  // without waiting for it.
  std::vector<uint8_t> pinnedPhis(scan.phis_.size(), 0);
  for (const il::BlockId blockId : function.blockHandles()) {
    const auto& ops = function.block(blockId).ops;
    if (ops.empty()) {
      continue;
    }
    const il::Op& terminator = function.op(ops.back());
    if (terminator.code != il::OpCode::IndirectBranch) {
      continue;
    }
    const auto targets = function.targets(terminator);
    if (targets.empty()) {
      continue;
    }
    const auto shape = matchDispatcherShape(function, blockId, targets);
    if (!shape.has_value()) {
      continue;
    }
    const auto dispatcherFrame = matchLiveRegisterFrame(function, *shape);
    if (!dispatcherFrame.has_value()) {
      continue;
    }
    for (const LiveRegisterSlot& slot : dispatcherFrame->slots) {
      const int index = activeAbi.argIndex(slot.reg);
      if (index < 0) {
        continue;
      }
      if (const auto found =
              table.tempByValue_.find(function.op(slot.livePhiAtHub).result.index());
          found != table.tempByValue_.end()) {
        table.temps_[found->second].name = std::format("x{}_live", index);
        pinnedPhis[found->second] = 1;
      }
      if (const auto found =
              table.tempByValue_.find(function.op(slot.shadowPhiAtMerge).result.index());
          found != table.tempByValue_.end()) {
        table.temps_[found->second].name = std::format("x{}_exit", index);
        pinnedPhis[found->second] = 1;
      }
    }
  }

  coalescePhis(function, scan.phis_, pinnedPhis, table.temps_, table.tempByValue_,
               table.tempValues_);

  return table;
}

void VariableTable::applyImportedTypes(const TypedVariables& typed,
                                       const types::TypeBinder& binder) {
  const types::TypeDatabase& database = binder.database();
  for (Variable& arg : args_) {
    const std::optional<types::TypeId> found = typed.forArgument(arg.argRoot);
    if (found.has_value() && binder.consistent(*found, arg.type.width, arg.type.pointerDepth)) {
      arg.importedType = *found;
    }
  }
  for (std::size_t tIdx = 0; tIdx < temps_.size(); ++tIdx) {
    Variable& temp = temps_[tIdx];
    std::optional<types::TypeId> agreed;
    bool conflict = false;
    const auto checkVal = [&](il::ValueId val) {
      const std::optional<types::TypeId> found = typed.forValue(val);
      if (found.has_value() &&
          binder.consistent(*found, temp.type.width, temp.type.pointerDepth)) {
        if (!agreed.has_value()) {
          agreed = *found;
        } else if (*agreed != *found) {
          conflict = true;
        }
      }
    };
    if (tIdx < tempValues_.size() && !tempValues_[tIdx].empty()) {
      for (const il::ValueId v : tempValues_[tIdx]) {
        checkVal(v);
      }
    } else {
      checkVal(temp.value);
    }
    if (!conflict && agreed.has_value()) {
      temp.importedType = *agreed;
    }
  }
  // Locals are ordered by ascending stackDelta (see recover(), which builds
  // them from a std::map keyed the same way). That is what makes a struct's
  // extent checkable against its neighbours: `struct timeval` claims 16
  // bytes at `sp-0x50`, and `sp-0x48` -- eight bytes in, already its own
  // Local because something loads it directly -- has to be accounted for
  // rather than silently overwritten. It is, when it lands on an exact field
  // (see absorb() below); otherwise the promotion is skipped rather than
  // printing a struct with an unexplained hole in it.
  for (std::size_t index = 0; index < locals_.size(); ++index) {
    Variable& local = locals_[index];
    const std::optional<types::TypeId> found = typed.forStackSlot(local.stackDelta);
    if (!found.has_value()) {
      continue;
    }
    const std::optional<uint64_t> size = database.sizeOf(*found);
    if (!size.has_value() || *size * 8 < local.type.width) {
      continue;  // narrower than what was actually read: the header disagrees
    }
    const int64_t base = local.stackDelta;
    const int64_t end = base + static_cast<int64_t>(*size);
    std::size_t last = index;
    bool coversCleanly = true;
    while (last + 1 < locals_.size() && locals_[last + 1].stackDelta < end) {
      const Variable& sibling = locals_[last + 1];
      const types::TypeDatabase::FieldPath field =
          database.fieldAt(*found, static_cast<uint64_t>(sibling.stackDelta - base),
                           sibling.type.width);
      if (!field.found() || field.remainder != 0 ||
          database.sizeOf(field.type).value_or(0) * 8 != sibling.type.width) {
        coversCleanly = false;
        break;
      }
      ++last;
    }
    if (!coversCleanly) {
      continue;
    }
    for (std::size_t absorbed = index + 1; absorbed <= last; ++absorbed) {
      const types::TypeDatabase::FieldPath field =
          database.fieldAt(*found, static_cast<uint64_t>(locals_[absorbed].stackDelta - base),
                           locals_[absorbed].type.width);
      std::string path;
      for (const std::string& name : field.names) {
        path += path.empty() ? name : "." + name;
      }
      locals_[absorbed].aliasBase = base;
      locals_[absorbed].aliasField = std::move(path);
    }
    local.importedType = *found;
  }
}

const Variable* VariableTable::argumentFor(il::RegId root) const {
  if (const auto found = argByRoot_.find(root.index()); found != argByRoot_.end()) {
    return &args_[found->second];
  }
  return nullptr;
}

const Variable* VariableTable::localAt(int64_t delta) const {
  if (const auto found = localByDelta_.find(delta); found != localByDelta_.end()) {
    return &locals_[found->second];
  }
  return nullptr;
}

const Variable* VariableTable::tempFor(il::ValueId value) const {
  if (const auto found = tempByValue_.find(value.index());
      found != tempByValue_.end()) {
    return &temps_[found->second];
  }
  return nullptr;
}

}  // namespace xdec::analysis
