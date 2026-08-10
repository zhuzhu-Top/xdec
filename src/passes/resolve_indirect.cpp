// resolve-indirect (see the header for the contract).
#include "xdec/passes/resolve_indirect.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>

#include "xdec/analysis/dominators.h"
#include "xdec/analysis/image_eval.h"
#include "xdec/analysis/index_bound.h"
#include "xdec/analysis/jump_table.h"
#include "xdec/il/function.h"
#include "xdec/il/printer.h"
#include "xdec/support/bits.h"
#include "xdec/support/log.h"

namespace xdec::passes {

namespace {

// Why a branch did not resolve.
//
// An unresolved indirect branch is a hard verification failure at Resolved, so
// this pass failing means the whole decompilation fails — and the message the
// verifier can give is only "still unresolved at 0x...", which says nothing
// about which of the several ways to fail happened. The distinctions matter and
// are not guessable from the outside: a branch with no table match and a top
// value set is a modelling gap in this pass, a branch whose table enumerated
// forty targets of which two were never lifted is a discovery problem, and the
// two want opposite responses. Set XDEC_LOG=resolve=debug to get them.
XDEC_DEFINE_LOG_CATEGORY(resolveLog, "resolve")

class ResolveIndirect final : public pass::FunctionPass {
 public:
  ResolveIndirect()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "resolve-indirect";
          info.level = il::Maturity::Ssa;
          info.produces = il::Maturity::Resolved;
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    const ByteReader* image = context.image();
    if (image == nullptr) {
      return Unexpected{std::make_unique<Diag>(
          DiagCode::Internal,
          "resolve-indirect reads the binary image; wire Manager::setImage to run "
          "past Ssa")};
    }

    il::Function& function = context.function();
    image_ = image;
    analysis::ImageEval eval(function, *image);
    // One tree for the whole pass, matching the edges as they stand now: this
    // pass does not rebuild them until it is done, so the snapshot stays
    // consistent with what every query below sees. Targets set during this run
    // are accounted for on the driver's next round, which recomputes everything.
    const analysis::Dominators dominators = analysis::Dominators::compute(function);

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.ops.empty()) {
        continue;
      }
      const il::OpId terminatorId = block.ops.back();
      const il::Op& terminator = function.op(terminatorId);
      if (terminator.code != il::OpCode::IndirectBranch ||
          !function.targets(terminator).empty()) {
        continue;
      }
      changed |= resolveOne(context, eval, dominators, blockId, terminatorId);
    }
    if (changed) {
      function.rebuildEdges();
    }
    if (context.sealUnresolvedBranches()) {
      // After rebuildEdges, so reachability reflects everything just resolved:
      // a branch is only a hole if the block it ends is one the entry reaches.
      changed |= seal(function);
    }
    return changed;
  }

 private:
  /// Replaces every branch this pass could not answer with an opaque
  /// terminator naming the target expression, so the function is emittable
  /// with its holes marked instead of discarded whole (see
  /// Context::setSealUnresolvedBranches).
  ///
  /// The expression survives as text, not IL, and that is the point: there is
  /// nothing left to analyse about it, only something to read. Unimplemented
  /// already means "control stops here and the effect is opaque" to every
  /// consumer downstream -- DCE, stack-prop and copy-prop each treat it as
  /// able to do anything -- which is exactly the truth about a jump to an
  /// address the image does not determine.
  /// Renders a branch target for a human, following one step through SSA.
  ///
  /// Printing the expression alone gives `val:i64(%13805)` on exactly the
  /// branches that need explaining, because by Ssa the address is a value and
  /// the shape that produced it lives in the defining op. One step is enough to
  /// show whether it was a table load, a call result or a stack slot, which is
  /// the question a reader has here.
  [[nodiscard]] static std::string describe(const il::Function& function,
                                            il::ExprId target) {
    const il::Expr& expr = function.expr(target);
    if (expr.op == il::ExprOp::Value) {
      const il::ValueId id{static_cast<uint32_t>(expr.immediate)};
      if (function.hasValue(id) && function.hasOp(function.value(id).definition)) {
        return il::printOp(function, function.value(id).definition);
      }
    }
    return il::printExpr(function, target);
  }

  [[nodiscard]] static bool seal(il::Function& function) {
    const analysis::Dominators dominators = analysis::Dominators::compute(function);
    std::vector<std::pair<il::BlockId, std::string>> holes;
    for (const il::BlockId blockId : function.blockHandles()) {
      const il::Block& block = function.block(blockId);
      if (block.ops.empty() || !dominators.reachable(blockId)) {
        continue;
      }
      const il::Op& terminator = function.op(block.ops.back());
      if (terminator.code != il::OpCode::IndirectBranch ||
          !function.targets(terminator).empty()) {
        continue;
      }
      const auto operands = function.operands(terminator);
      holes.emplace_back(
          blockId, std::format("unresolved indirect branch at {:#x} to {}", terminator.va,
                               operands.empty() ? std::string{"an unrecorded expression"}
                                                : describe(function, operands[0])));
    }
    for (const auto& [blockId, description] : holes) {
      const uint64_t va = function.op(function.block(blockId).ops.back()).va;
      function.dropTerminator(blockId);
      (void)function.appendUnimplemented(blockId, va, description);
    }
    if (!holes.empty()) {
      XDEC_LOG_WARN(resolveLog(),
                    "sealed {} unresolved indirect branch(es) as opaque; the function "
                    "is emitted with those holes marked",
                    holes.size());
      function.rebuildEdges();
    }
    return !holes.empty();
  }

  /// Collect candidates from both paths, then resolve all-or-nothing: every
  /// candidate must land on an existing block, or the branch keeps its
  /// unresolved state and every missing address is reported for the driver.
  bool resolveOne(pass::Context& context, analysis::ImageEval& eval,
                  const analysis::Dominators& dominators, il::BlockId blockId,
                  il::OpId terminatorId) {
    il::Function& function = context.function();
    const auto operands = function.operands(function.op(terminatorId));
    if (operands.empty()) {
      return false;
    }

    // Whole-table enumeration leads: a jump table's entries are all valid
    // targets whatever the index computes -- except that tableCandidates now
    // reads only the indices the index expression can actually take when it
    // can prove a finite set of them (see its own comment), so "whatever the
    // index computes" only means "every structurally possible one" when
    // nothing narrower is provable. The value set is consulted second, and
    // only when the table path found nothing at all: where both succeed they
    // are reading the same underlying address computation and agree, so a
    // union would only ever add duplicates; where the table path aborted
    // because an entry misread, a stale value set filling in the gap would
    // hide exactly the mismatch that abort exists to report.
    std::vector<uint64_t> candidates =
        tableCandidates(context, eval, dominators, blockId, operands[0]);
    const std::size_t fromTable = candidates.size();
    if (candidates.empty()) {
      candidates = valueSetCandidates(eval, operands[0]);
    }
    const uint64_t branchVa = function.op(terminatorId).va;
    if (candidates.empty()) {
      // The expression itself, because a shape that did not match is a question
      // about what the shape actually was, and no summary of it substitutes for
      // reading it.
      XDEC_LOG_DEBUG(resolveLog(), "{:#x}: no candidates -- {}, value set {}: {}",
                     branchVa,
                     analysis::matchJumpTable(function, operands[0], singleValue(eval))
                         ? "table matched but enumerated nothing"
                         : "no table shape matched",
                     eval.eval(operands[0]).isTop() ? "is top" : "is empty",
                     il::printExpr(function, operands[0]));
      return false;
    }

    std::vector<il::BlockId> blocks;
    std::vector<uint64_t> missing;
    for (const uint64_t va : candidates) {
      const il::BlockId target = function.blockAt(va);
      if (target.valid()) {
        blocks.push_back(target);
      } else {
        missing.push_back(va);
      }
    }
    if (!missing.empty()) {
      // Code the lifter never reached: report every gap at once so one
      // driver round lifts the whole table, not one entry per round. The whole
      // candidate set goes with it, because it is the edge the driver puts back
      // once those blocks exist -- proved here, on evidence that will not be
      // available there (see pass::Discovery).
      XDEC_LOG_DEBUG(resolveLog(),
                     "{:#x}: {} candidate(s) ({} from a table), {} not yet lifted, "
                     "first {:#x}",
                     branchVa, candidates.size(), fromTable, missing.size(),
                     missing.front());
      context.reportDiscovery(pass::Discovery{.branch = branchVa,
                                              .targets = std::move(candidates),
                                              .missing = std::move(missing)});
      return false;
    }
    XDEC_LOG_TRACE(resolveLog(), "{:#x}: resolved to {} target(s)", branchVa,
                   blocks.size());
    function.setTargets(terminatorId, blocks);
    // The branch goes to blocks in this function, so the argument snapshot SSA
    // construction left on it (in case it was a tail call -- see
    // recover-tailcall) is answered and gone. Dropping it is not tidiness: kept,
    // those operands are uses, and the register setup they hold alive would look
    // like arguments to `vars` and stop dead code elimination from collecting a
    // dispatcher's stale register writes.
    const auto current = function.operands(function.op(terminatorId));
    if (current.size() > 1) {
      const il::ExprId kept[] = {current[0]};
      function.setOperands(terminatorId, kept);
    }
    return true;
  }

  /// Path one: the bounded value set, for selects over a handful of tables
  /// and single global pointers.
  std::vector<uint64_t> valueSetCandidates(analysis::ImageEval& eval,
                                           il::ExprId target) {
    const analysis::ValueSet set = eval.eval(target);
    if (set.isTop() || set.values().empty()) {
      return {};
    }
    std::vector<uint64_t> out;
    for (const uint64_t va : set.values()) {
      // Zero for the same reason a table's null slot is not an entry: an
      // unrelocated pointer slot reads as zero, and zero is not code.
      if (va != 0 && readable(va)) {
        out.push_back(va);
      }
    }
    return out;
  }

  /// Resolves a table's base or anchor when it is one value that is not written
  /// as one (see analysis::ConstantResolver). Sound by construction: the
  /// evaluator's set is every value the expression can take, so a singleton is
  /// the value, not a sample of several.
  [[nodiscard]] static analysis::ConstantResolver singleValue(analysis::ImageEval& eval) {
    return [&eval](il::ExprId id) -> std::optional<uint64_t> {
      const analysis::ValueSet set = eval.eval(id);
      if (set.isTop() || set.values().size() != 1) {
        return std::nullopt;
      }
      return set.values()[0];
    };
  }

  /// Path two: whole-table enumeration, for dispatchers whose index is not
  /// statically knowable -- and, first, the narrower answer for dispatchers
  /// whose index *is*: read only the entries it can actually select.
  ///
  /// A table's shape says every entry is a valid branch target; it says
  /// nothing about which entries this particular branch, with this
  /// particular index expression, can ever reach. A branchless clamp
  /// (`state > k ? replacement : state`, see analysis/index_bound.h's
  /// localBound for the structural version of the same reasoning) or a flag
  /// zero-extended straight into the index can make the index's own value set
  /// -- analysis/image_eval.h, the same evaluator every other pass in this
  /// file already trusts -- a handful of concrete numbers well inside what
  /// the structural bound merely allows. Reading only those is the difference
  /// between a dispatcher's real cases and a `switch` with a live case for
  /// every slot an obfuscator's dead state, padding, or neighboring table
  /// happened to leave bound-reachable.
  ///
  /// How far to enumerate when the value set does not narrow anything (it is
  /// `top`, or every value it offers gets discarded below) is the older
  /// difficulty, and there are two answers. The good one is the guard that
  /// bounds the index (analysis/index_bound.h): it states the length, so
  /// every entry up to it is read and any entry that is then not code means
  /// the table was misread and nothing is claimed.
  ///
  /// Without a guard, all that is left is to read until an entry stops looking
  /// like a target, which does not find the end so much as stumble over
  /// something -- so it is bounded, and running out means the attempt failed
  /// rather than that the table is kMaxEntries long. Truncating instead was what
  /// this pass used to do, and it cost this sample 1900 blocks of decoded data,
  /// four hundred of them unreachable, one of which held an indirect branch that
  /// could never resolve and so failed the whole decompilation.
  std::vector<uint64_t> tableCandidates(pass::Context& context, analysis::ImageEval& eval,
                                        const analysis::Dominators& dominators,
                                        il::BlockId blockId, il::ExprId target) {
    const il::Function& function = context.function();
    const auto table = analysis::matchJumpTable(function, target, singleValue(eval));
    if (!table || table->stride == 0) {
      return {};
    }
    // A bare base carries no index (see analysis::JumpTable), which makes it a
    // pointer, not a table: `load(g)` is one target, the one in that slot. What
    // follows the slot is whatever the linker put next, and in .data.rel.ro
    // that is more relocated function pointers -- so enumerating from here
    // reads a run of perfectly code-looking addresses and claims every one of
    // them as a target of a branch that has exactly one. The value set reads
    // the slot and stops, which is the right answer and already the path this
    // shape takes.
    if (!table->index.valid()) {
      return {};
    }

    // A bound is on the index, so the entry count is one more than it.
    const std::optional<uint64_t> proven =
        analysis::boundOnIndex(function, dominators, blockId, table->index);
    const ByteReader& image = *context.image();

    // The precise path: the index's own value set, when it is not top. A
    // value the structural bound does not admit means the two proofs
    // disagree about this table, which is a reason to distrust the precise
    // set (fall through to the structural path below) rather than to pick a
    // winner between two claims that cannot both be right. An entry that does
    // not read as code is the same signal path two already treats as
    // "misread the table, claim nothing" -- the value set being wrong about
    // an index is not a reason to make up an answer for it, either.
    const analysis::ValueSet indexValues = eval.eval(table->index);
    if (!indexValues.isTop() && !indexValues.values().empty()) {
      std::vector<uint64_t> preciseIndices(indexValues.values().begin(),
                                           indexValues.values().end());
      std::sort(preciseIndices.begin(), preciseIndices.end());
      std::vector<uint64_t> out;
      bool trustworthy = true;
      for (const uint64_t index : preciseIndices) {
        if (proven.has_value() && index > *proven) {
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: the index's value set includes {} but the guard "
                         "bounds it to {}; the two disagree, so the value set is not "
                         "trusted and the structural bound is used instead",
                         table->base, index, *proven);
          trustworthy = false;
          break;
        }
        const uint64_t va = entryTarget(image, *table, index);
        if (!table->relative && va == 0) {
          continue;
        }
        if (va == kNoTarget || !isCode(context, va)) {
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: entry {} from the index's value set is not "
                         "code, so the value set is being misread; falling back to the "
                         "structural bound",
                         table->base, index);
          trustworthy = false;
          break;
        }
        out.push_back(va);
      }
      if (trustworthy && !out.empty()) {
        XDEC_LOG_DEBUG(resolveLog(),
                       "table at {:#x}: {} of its entries are the ones the index {} can "
                       "actually take",
                       table->base, out.size(), il::printExpr(function, table->index));
        return out;
      }
    }

    const uint64_t limit =
        proven.has_value() ? *proven + 1 : uint64_t{kMaxEntries};

    std::vector<uint64_t> out;
    for (uint64_t index = 0; index <= limit; ++index) {
      if (index == limit) {
        if (proven.has_value()) {
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: {} entries, from the guard bounding its index",
                         table->base, limit);
          break;
        }
        XDEC_LOG_DEBUG(resolveLog(),
                       "table at {:#x} (stride {}, {}-bit entries) has no end within "
                       "{} entries and nothing bounds its index; enumerating it is "
                       "guesswork, so nothing is claimed",
                       table->base, table->stride, table->entryBits, kMaxEntries);
        return {};
      }
      // A rejected entry means two different things. Where the length was proved,
      // every index below it is an entry, so one that does not read as a target
      // says the table was misread — wrong stride, wrong entry width, wrong base
      // — and the honest response is to claim none of it. Where the length was
      // not proved, a rejected entry is the only end marker there is.
      const uint64_t va = entryTarget(image, *table, index);
      // A null slot in a pointer table is an index nothing dispatches on, and
      // it is neither a target nor an end. Not a target because address zero is
      // not code -- a shared object's first segment can start at vaddr 0 and be
      // mapped executable, so the plausibility test below waves it through and
      // the CFG grows an edge to the ELF header. Not an end because a slot the
      // linker never filled says nothing about where the table stops; treating
      // it as the end is what makes an unbounded scan claim the prefix in front
      // of it, which is the guesswork this path refuses to do everywhere else.
      if (!table->relative && va == 0) {
        continue;
      }
      if (va == kNoTarget || !isCode(context, va)) {
        if (proven.has_value()) {
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: entry {} of a proven {} is not code, so the "
                         "table is being read wrong; nothing is claimed",
                         table->base, index, limit);
          return {};
        }
        break;
      }
      out.push_back(va);
    }
    return out;
  }

  /// Where entry `index` points, or kNoTarget when it cannot be an entry: the
  /// slot is unreadable, the target is not instruction-aligned, or a relative
  /// entry lands implausibly far from its anchor.
  [[nodiscard]] static uint64_t entryTarget(const ByteReader& image,
                                            const analysis::JumpTable& table,
                                            uint64_t index) {
    const uint64_t slot = table.base + index * table.stride;
    std::array<std::byte, 8> bytes{};
    const std::size_t width = table.entryBits / 8;
    if (!image(slot, std::span<std::byte>(bytes).subspan(0, width))) {
      return kNoTarget;
    }
    uint64_t entry = 0;
    for (std::size_t at = 0; at < width; ++at) {
      entry |= static_cast<uint64_t>(bytes[at]) << (at * 8);
    }
    const uint64_t offset =
        (table.signedOffsets ? static_cast<uint64_t>(signExtend(entry, table.entryBits))
                             : entry)
        << table.offsetShift;
    const uint64_t va = table.relative ? table.anchor + offset : entry;
    if (va % 4 != 0) {
      return kNoTarget;
    }
    if (table.relative) {
      const uint64_t span = va > table.anchor ? va - table.anchor : table.anchor - va;
      if (span > kMaxOffsetSpan) {
        return kNoTarget;  // an offset this large is data, not a jump target
      }
    }
    return va;
  }

  [[nodiscard]] bool readable(uint64_t va) const {
    std::array<std::byte, 4> word{};
    return (*image_)(va, word).hasValue();
  }

  /// Whether `va` could be a branch target: the program executes it.
  ///
  /// This is what ends a table. Readability does not: past its last entry a
  /// table is followed by more data, and a word of .rodata read as an offset
  /// lands somewhere readable about as often as not, so a readability bound
  /// walks straight off the end. Executability is the property a target has and
  /// a stray offset almost never does.
  ///
  /// Falls back to readability when the pipeline states no permissions, because
  /// a caller that wires no facts would otherwise resolve nothing at all — the
  /// bound is then as weak as it was, which is a caller's choice and not a wrong
  /// answer.
  [[nodiscard]] bool isCode(const pass::Context& context, uint64_t va) const {
    const MemoryFacts& facts = context.memoryFacts();
    return facts.executable ? facts.isExecutable(va) : readable(va);
  }

  const ByteReader* image_ = nullptr;

  /// Not a valid target under any reading: no instruction is at an odd address.
  static constexpr uint64_t kNoTarget = 1;

  /// How far an unbounded enumeration will read before giving up. Only reached
  /// when no guard bounds the index; a proven length is honoured whatever it is,
  /// and the samples here have one of 1351.
  static constexpr uint32_t kMaxEntries = 512;
  /// Pc-relative tables keep their targets close to the anchor.
  static constexpr uint64_t kMaxOffsetSpan = 64ull << 20;
};

}  // namespace

std::unique_ptr<pass::Pass> makeResolveIndirectPass() {
  return std::make_unique<ResolveIndirect>();
}

}  // namespace xdec::passes
