// makeRecoverTailCallPass: rewrites an indirect branch through a pointer this
// function did not compute into the call and return it is (see the header for
// the evidence each shape rests on).
#include "xdec/passes/recover_tailcall.h"

#include <format>
#include <set>
#include <string>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"
#include "xdec/support/log.h"

namespace xdec::passes {

namespace {

XDEC_DEFINE_LOG_CATEGORY(tailLog, "tailcall")

/// How far to follow the target expression before giving up. A destination
/// computed through more hops than this is not a function pointer somebody
/// passed in, and refusing to answer is the safe answer.
constexpr unsigned kMaxDepth = 12;

/// Where an object's address can start (see addressesImage).
constexpr uint64_t kFirstPage = 0x1000;

/// What the walk of a branch target found. Three independent observations, none
/// of which is a decision on its own -- see classify() for how they combine.
///
/// The first two are about the destination's *address chain* only (see walk):
/// an argument register that indexes a table is not the table's base, and a
/// constant that scales an index is not an address.
struct Origin {
  /// An argument register's entry value is in there: a pointer the caller owns.
  bool callerValue = false;
  /// So is a constant that addresses this image: the mark of a jump table,
  /// whose base the compiler wrote into the instruction stream.
  bool imageAddress = false;
  /// The destination is read from a slot the loader binds to another module.
  bool importSlot = false;
  std::string importName;
};

class RecoverTailCall final : public pass::FunctionPass {
 public:
  RecoverTailCall()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "recover-tailcall";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Ssa;
          // Without copy propagation the target of `mov x3, x0; br x3` is a
          // register read, and the argument shuffle in between hides the entry
          // leaf this pass looks for.
          info.requirements = {"ssa-optimize"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    argRoots_.clear();
    for (unsigned index = 0; index < 8; ++index) {
      const il::RegId reg = function.registers().find(std::format("x{}", index));
      if (reg.valid()) {
        argRoots_.insert(function.registers().rootOf(reg));
      }
    }
    if (argRoots_.empty()) {
      // An architecture whose argument registers are not named this way says
      // nothing about what a caller passed, and the whole decision rests on
      // that. Nothing is claimed and nothing is rewritten.
      return false;
    }

    // Unreachable blocks are swept-up data, not code (the same reasoning the
    // verifier states for not demanding their branches resolve). SSA
    // construction skips them too, so their branch targets are register reads
    // no analysis can see through, and rewriting one would invent a call.
    const analysis::Dominators dominators = analysis::Dominators::compute(function);

    bool changed = false;
    // By handle rather than over rpo(): the rewrite appends ops, and iterating
    // a structure this edits is only safe because block handles are stable.
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.ops.empty() || !dominators.reachable(blockId)) {
        continue;
      }
      const il::OpId terminatorId = block.ops.back();
      const il::Op& terminator = function.op(terminatorId);
      if (terminator.code != il::OpCode::IndirectBranch ||
          !function.targets(terminator).empty()) {
        continue;
      }
      changed |= recoverOne(context, blockId, terminatorId);
    }
    return changed;
  }

 private:
  bool recoverOne(pass::Context& context, il::BlockId blockId, il::OpId terminatorId) {
    il::Function& function = context.function();
    const auto operands = function.operands(function.op(terminatorId));
    if (operands.empty()) {
      return false;
    }
    const il::ExprId target = operands[0];
    const uint64_t branchVa = function.op(terminatorId).va;

    Origin origin;
    std::set<uint32_t> visited;
    // The target starts in address position: it is where control goes.
    walk(context, function, target, 0, true, visited, origin);

    if (!classify(origin)) {
      XDEC_LOG_DEBUG(tailLog(),
                     "{:#x}: not a tail call -- caller value {}, image address {}: {}",
                     branchVa, origin.callerValue, origin.imageAddress,
                     il::printExpr(function, target));
      return false;
    }

    // The ABI snapshot: operands past the target are the argument registers'
    // versions at the branch, recorded by SSA construction for exactly this.
    // Absent (an IL written by hand, or a branch SSA construction never saw),
    // the call is emitted with no arguments rather than invented ones.
    const std::vector<il::ExprId> args(operands.begin() + 1, operands.end());

    const std::string note =
        origin.importSlot
            ? std::format("tail call through the '{}' import slot", origin.importName)
            : std::string("tail call through a pointer the caller passed in");
    XDEC_LOG_DEBUG(tailLog(), "{:#x}: {} ({} argument(s))", branchVa, note, args.size());

    // A tail call's result is its caller's result, so the call is typed as one
    // returning a machine word and the return hands that value straight back.
    // Where a prototype says otherwise, `apply-types` and the emitter narrow it;
    // where nothing does, a word is what the ABI leaves in x0.
    function.dropTerminator(blockId);
    const il::OpId callId =
        function.appendCall(blockId, branchVa, target, il::Type::integer(64));
    if (!args.empty()) {
      std::vector<il::ExprId> callOperands;
      callOperands.reserve(args.size() + 1);
      callOperands.push_back(target);
      callOperands.insert(callOperands.end(), args.begin(), args.end());
      function.setOperands(callId, callOperands);
    }
    function.annotate(callId, note);

    const il::OpId returnId = function.appendReturn(blockId, branchVa);
    const il::ValueId result = function.resultOf(callId);
    if (result.valid()) {
      const il::ExprId value = function.valueRef(result);
      const il::ExprId returned[] = {value};
      function.setOperands(returnId, returned);
    }
    return true;
  }

  /// The one judgement in this pass, kept in one place.
  ///
  /// An address in this image outranks everything else the walk found: a table
  /// indexed by a parameter mentions that parameter, so "the caller's value is
  /// in there" cannot mean tail call while a base this image wrote down is in
  /// there too. A bound import slot is the exception, and not an inconsistent
  /// one -- the constant in that shape is the slot, and a slot the loader fills
  /// at run time holds no address this image chose.
  [[nodiscard]] static bool classify(const Origin& origin) {
    if (origin.importSlot) {
      return true;
    }
    return origin.callerValue && !origin.imageAddress;
  }

  /// Follows the target expression, and a loaded value into the address it came
  /// from, recording what the destination is built out of.
  ///
  /// `addressing` says whether this subexpression is part of the address chain
  /// or merely a number feeding into it, and it is what keeps a shift amount
  /// from being read as a table base. An address is a base plus offsets, so the
  /// property survives Add, Sub, Or and casts; every other operator computes a
  /// number, and a constant under one of those is a scale, a mask or a stride
  /// whatever its value.
  void walk(const pass::Context& context, const il::Function& function, il::ExprId id,
            unsigned depth, bool addressing, std::set<uint32_t>& visited, Origin& out) const {
    // Keyed on the expression and the position it was reached in: the same
    // interned constant can be a base in one parent and a stride in another.
    const uint32_t key = (id.index() << 1) | static_cast<uint32_t>(addressing);
    if (depth > kMaxDepth || !visited.insert(key).second) {
      return;
    }
    const il::Expr expr = function.expr(id);  // by value: interning may dangle
    switch (expr.op) {
      case il::ExprOp::EntryReg:
        if (addressing && argRoots_.contains(il::RegId{static_cast<uint32_t>(expr.immediate)})) {
          out.callerValue = true;
        }
        return;
      case il::ExprOp::Const:
        if (addressing && addressesImage(context, expr.immediate)) {
          out.imageAddress = true;
        }
        return;
      case il::ExprOp::Value: {
        // A load is the only definition worth following: the address it read
        // from is where the destination came from, and the rest of the walk
        // asks the same questions about it.
        const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
        const il::OpId definition = function.value(value).definition;
        if (!definition.valid()) {
          return;
        }
        const il::Op& op = function.op(definition);
        if (op.code != il::OpCode::Load) {
          return;
        }
        const auto address = function.operands(op);
        if (address.empty()) {
          return;
        }
        uint64_t slot = 0;
        if (function.asConstantThroughCasts(address[0], slot)) {
          const LoaderValue bound = context.memoryFacts().loaderValueAt(slot);
          if (!bound.importName.empty()) {
            out.importSlot = true;
            out.importName = bound.importName;
            return;
          }
        }
        walk(context, function, address[0], depth + 1, true, visited, out);
        return;
      }
      case il::ExprOp::Select:
        // A choice between two destinations is still a destination, so both arms
        // are in the same position as the select. The condition is not: what
        // picked the pointer says nothing about where the pointer came from, and
        // counting a parameter there as "the caller supplied the destination" is
        // what would call `fn = cond ? f : g; return fn(a, b)` a tail call
        // through a caller's pointer when both arms are addresses right here.
        walk(context, function, expr.operands[0], depth + 1, false, visited, out);
        walk(context, function, expr.operands[1], depth + 1, addressing, visited, out);
        walk(context, function, expr.operands[2], depth + 1, addressing, visited, out);
        return;
      default:
        break;
    }
    const bool inherit = addressing && addressForming(expr.op);
    for (unsigned index = 0; index < expr.operandCount; ++index) {
      walk(context, function, expr.operands[index], depth + 1, inherit, visited, out);
    }
  }

  /// Operators an address chain is built from: a base, displacements, and the
  /// width adjustments a 32-bit index brings. `Or` is in because a compiler
  /// emits it for a displacement it can prove disjoint from the base's bits.
  [[nodiscard]] static bool addressForming(il::ExprOp op) {
    switch (op) {
      case il::ExprOp::Add:
      case il::ExprOp::Sub:
      case il::ExprOp::Or:
      case il::ExprOp::ZExt:
      case il::ExprOp::SExt:
      case il::ExprOp::Trunc:
      case il::ExprOp::Bitcast:
        return true;
      default:
        return false;
    }
  }

  /// Whether a constant is an address in this image, which is what a jump
  /// table's base is and what a caller's pointer is not.
  ///
  /// Readability is the test, because that is the claim: a constant the image
  /// can be read at is an address this image chose. Absent an image nothing is
  /// readable, and this pass then never sees a table base -- which is why it
  /// declines rather than acts when the pipeline wires no bytes.
  ///
  /// Below the first page is exempt, and not as a fudge. A shared object's
  /// segments start at a page boundary and a PIE's run-time base is a page
  /// address, so no object in it is addressed by a number this small; what
  /// numbers this small are, in an address chain, is field displacements. An
  /// image whose first segment happens to be mapped at zero (which is how an
  /// unrelocated ELF reads) would otherwise make every `p->field` look like a
  /// table base.
  [[nodiscard]] static bool addressesImage(const pass::Context& context, uint64_t value) {
    const ByteReader* image = context.image();
    if (image == nullptr || value < kFirstPage) {
      return false;
    }
    std::byte probe{};
    return (*image)(value, std::span<std::byte>{&probe, 1}).hasValue();
  }

  std::set<il::RegId> argRoots_;
};

}  // namespace

std::unique_ptr<pass::Pass> makeRecoverTailCallPass() {
  return std::make_unique<RecoverTailCall>();
}

}  // namespace xdec::passes
