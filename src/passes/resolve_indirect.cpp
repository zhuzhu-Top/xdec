// resolve-indirect (see the header for the contract).
#include "xdec/passes/resolve_indirect.h"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xdec/analysis/dominators.h"
#include "xdec/analysis/image_eval.h"
#include "xdec/analysis/index_bound.h"
#include "xdec/analysis/jump_table.h"
#include "xdec/binary/cache_pointer.h"
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
    analysis::ImageEval eval(function, *image, context.entryRegFacts());
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

  /// Where in `block` this branch reads the table it dispatches through: the
  /// address of the last memory read in `block` that `target` depends on,
  /// following values transitively. Nullopt when the expression reads no
  /// memory this block defines -- the read happened somewhere else, and
  /// nothing about where a target lands inside this block can skip it.
  [[nodiscard]] static std::optional<uint64_t> lastTableReadIn(const il::Function& function,
                                                               il::BlockId block,
                                                               il::ExprId target) {
    std::optional<uint64_t> read;
    std::vector<il::ExprId> work{target};
    std::vector<bool> seenValue(function.valueCount(), false);
    while (!work.empty()) {
      const il::Expr& expr = function.expr(work.back());
      work.pop_back();
      if (expr.op == il::ExprOp::Value) {
        const il::ValueId value{static_cast<uint32_t>(expr.immediate)};
        if (!function.hasValue(value) || seenValue[value.index()]) {
          continue;
        }
        seenValue[value.index()] = true;
        const il::ValueInfo& info = function.value(value);
        if (info.block != block || !function.hasOp(info.definition)) {
          continue;
        }
        const il::Op& definition = function.op(info.definition);
        if (definition.code == il::OpCode::Load && definition.va != il::kNoOpAddress) {
          read = read ? std::max(*read, definition.va) : definition.va;
        }
        for (const il::ExprId operand : function.operands(definition)) {
          work.push_back(operand);
        }
        continue;
      }
      for (uint32_t index = 0; index < expr.operandCount; ++index) {
        work.push_back(expr.operands[index]);
      }
    }
    return read;
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
    const uint64_t branchVa = function.op(terminatorId).va;
    // A candidate landing between this branch's own table read and the branch
    // is not a target. Taking it would re-run the arithmetic that turns an
    // entry into an address -- without re-reading the entry -- on the address
    // that arithmetic just produced: an anchor-relative table would compute
    // `target + anchor` and land nowhere at all. A candidate at or before the
    // read is a different thing entirely and stays: that one re-reads the
    // table, which is just an ordinary dispatch loop.
    //
    // Dropping the entry costs at most one wrong target and buys much more:
    // the self edge it would otherwise create puts a phi in front of the
    // table load, and a table behind a phi is one no later analysis
    // recognises as a table at all -- not matchJumpTable, so not the
    // index-mode switch, and not findDispatchRegions either.
    const std::optional<uint64_t> tableRead = lastTableReadIn(function, blockId, operands[0]);
    const auto skipsOwnTableRead = [&](uint64_t va) {
      return tableRead.has_value() && va > *tableRead && va <= branchVa;
    };

    std::vector<uint64_t> candidates =
        tableCandidates(context, eval, dominators, blockId, operands[0]);
    std::erase_if(candidates, skipsOwnTableRead);
    const std::size_t fromTable = candidates.size();
    if (candidates.empty()) {
      candidates = valueSetCandidates(eval, operands[0]);
      std::erase_if(candidates, skipsOwnTableRead);
    }
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
    static constexpr binary::CachePointerDecoder kCachePointer;
    std::vector<uint64_t> out;
    for (const uint64_t va : set.values()) {
      // Zero for the same reason a table's null slot is not an entry: an
      // unrelocated pointer slot reads as zero, and zero is not code.
      if (va == 0) {
        continue;
      }
      if (readable(va)) {
        out.push_back(va);
        continue;
      }
      // Same tagged-pointer fallback as the table path (entriesFor above):
      // a value this evaluator read straight out of image bytes may be a
      // shared-cache table entry that the table matcher itself did not
      // recognise as a table.
      if (const uint64_t decoded = kCachePointer.decode(va); decoded != va && readable(decoded)) {
        out.push_back(decoded);
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

  /// Path two: table enumeration only when the index is provably finite.
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
  /// Where the index is computed from something no evaluator can see through
  /// -- a call's return value, a stack canary -- its *shape* still narrows it
  /// (analysis/index_bound.h's preciseIndexSet), and that is the second thing
  /// tried, for the same reason and to the same standard as the first.
  ///
  /// When neither the value set nor the shape names a finite candidate set, the
  /// only remaining precise answer is a guard that bounds the index
  /// (analysis/index_bound.h): every index from zero through that bound is
  /// read, and any entry that is then not code means the table was misread.
  /// There is no third path that scans until bytes stop looking like targets --
  /// that guesswork claimed hundreds of spurious edges (absd @0x100023688) and
  /// tripped discovery caps without ever matching the branch's real arity.
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

    // The precise paths: enumerate the entries the index can actually select,
    // rather than every entry a length admits. A value the structural bound
    // does not admit means the two proofs disagree about this table, which is
    // a reason to distrust the precise set (fall through to the next source)
    // rather than to pick a winner between two claims that cannot both be
    // right, whichever path found the set.
    //
    // An entry that does not read as code is read differently by the two
    // paths, though. The evaluator's value set is a claim about which values
    // occur, full stop -- a bad entry means that claim was wrong, so the
    // proven-length path below treats it as "misread the table, claim
    // nothing" and so does this one. The shape-derived set is a weaker claim:
    // preciseIndexSet proves which values an expression's *structure* admits,
    // not which of them this call site's calling context can actually reach
    // -- two independent shr.u(x,31)-shaped bits combine to four values where
    // only two are live, because the fourth bit is fixed by a dominating
    // dispatch this analysis does not look at. A bad entry there is exactly
    // that fourth bit's combination, not a reason to distrust the other
    // three -- so it is dropped, not treated as proof the whole set is wrong.
    const auto entriesFor = [&](std::span<const uint64_t> indices, std::string_view provenance,
                                bool tolerateDeadCombinations) -> std::vector<uint64_t> {
      std::vector<uint64_t> out;
      for (const uint64_t index : indices) {
        if (proven.has_value() && index > *proven) {
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: {} includes {} but the guard bounds the index "
                         "to {}; the two disagree, so it is not trusted",
                         table->base, provenance, index, *proven);
          return {};
        }
        uint64_t va = entryTarget(image, *table, index);
        if (!table->relative && va == 0) {
          continue;
        }
        // A computed target that does not read as code might still be a
        // tagged shared-cache pointer rather than a misread table entry --
        // see binary::CachePointerDecoder. Masking the fully-computed va
        // (rather than just the raw entry) also covers an offset table whose
        // *anchor* was itself read from a tagged slot: `tag|anchor + offset`
        // masks down to `anchor + offset` exactly, since a table's offsets
        // are bounded far below the tag's own low bit (kMaxOffsetSpan is
        // 64 MiB; the tag starts above bit 34, comfortably clear of any
        // carry). Only kept when the decoded address itself checks out; a
        // table that was genuinely misread stays misread.
        if (va != kNoTarget && !isCode(context, va)) {
          static constexpr binary::CachePointerDecoder kCachePointer;
          if (const uint64_t decoded = kCachePointer.decode(va); decoded != va && isCode(context, decoded)) {
            XDEC_LOG_DEBUG(resolveLog(),
                           "table at {:#x}: entry {} ({:#x}) is not code, but its low bits "
                           "({:#x}) are -- treating it as a tagged shared-cache pointer "
                           "(tag {:#x})",
                           table->base, index, va, decoded, kCachePointer.tag(va));
            va = decoded;
          }
        }
        if (va == kNoTarget || !isCode(context, va)) {
          if (tolerateDeadCombinations) {
            XDEC_LOG_DEBUG(resolveLog(),
                           "table at {:#x}: entry {} from {} is not code; treated as a "
                           "combination this call site cannot reach, not as a misread table",
                           table->base, index, provenance);
            continue;
          }
          XDEC_LOG_DEBUG(resolveLog(),
                         "table at {:#x}: entry {} from {} is not code, so it is being "
                         "misread; falling back",
                         table->base, index, provenance);
          return {};
        }
        out.push_back(va);
      }
      if (!out.empty()) {
        XDEC_LOG_DEBUG(resolveLog(),
                       "table at {:#x}: {} of its entries are the ones the index {} can "
                       "actually take, per {}",
                       table->base, out.size(), il::printExpr(function, table->index),
                       provenance);
      }
      return out;
    };

    // Concrete values first, from the evaluator every other resolver in this
    // file already trusts.
    const analysis::ValueSet indexValues = eval.eval(table->index);
    if (!indexValues.isTop() && !indexValues.values().empty()) {
      std::vector<uint64_t> preciseIndices(indexValues.values().begin(),
                                           indexValues.values().end());
      std::sort(preciseIndices.begin(), preciseIndices.end());
      // Same tolerance as the shape path below: a phi/select may union a
      // live small index with a stale EntryReg arm (absd @0x100023688 is the
      // case -- w22 is recomputed at 0x1000235d4 on the taken path but another
      // predecessor still reads EntryReg). Dropping indices whose slots do not
      // read as code keeps {0, 2} from ever claiming 1; it also keeps a huge
      // dead EntryReg index from aborting the whole set when a small live one
      // is in the same union.
      if (std::vector<uint64_t> out =
              entriesFor(preciseIndices, "the index's value set", /*tolerateDeadCombinations=*/true);
          !out.empty()) {
        return out;
      }
    }

    // Then the index's shape, which answers where concrete values cannot
    // (analysis/index_bound.h's preciseIndexSet). This is the path an
    // obfuscated dispatch takes: an index computed from a call's return value
    // is top to any evaluator that needs the value, and still only ever two
    // numbers once the shift and the OR around it are read.
    if (const std::optional<std::vector<uint64_t>> shaped =
            analysis::preciseIndexSet(function, table->index)) {
      if (std::vector<uint64_t> out =
              entriesFor(*shaped, "the index's shape", /*tolerateDeadCombinations=*/true);
          !out.empty()) {
        return out;
      }
    }

    if (!proven.has_value()) {
      XDEC_LOG_DEBUG(resolveLog(),
                     "table at {:#x}: index {} is not guard-bounded and neither the value "
                     "set nor the index's shape named any target; nothing is claimed",
                     table->base, il::printExpr(function, table->index));
      return {};
    }

    std::vector<uint64_t> guardedIndices;
    guardedIndices.reserve(*proven + 1);
    for (uint64_t index = 0; index <= *proven; ++index) {
      guardedIndices.push_back(index);
    }
    return entriesFor(guardedIndices, "the guard bounding its index",
                      /*tolerateDeadCombinations=*/false);
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
  /// Pc-relative tables keep their targets close to the anchor.
  static constexpr uint64_t kMaxOffsetSpan = 64ull << 20;
};

}  // namespace

std::unique_ptr<pass::Pass> makeResolveIndirectPass() {
  return std::make_unique<ResolveIndirect>();
}

}  // namespace xdec::passes
