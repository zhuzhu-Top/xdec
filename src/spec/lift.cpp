// liftBasicBlock: the probe-then-elaborate flow over one basic block.
// liftFunction: recursive-descent function discovery on top of it.
// liftBlocksInto: the same discovery, spliced into an existing function.
//
// Both function-level entry points share the Discovery machine below: phase
// one scans block extents by probing only (no IL is built), phase two creates
// blocks and elaborates. The difference is only what the machine starts with:
// liftFunction starts from an empty function, liftBlocksInto pre-seeds it
// with the blocks already lifted, so new scans stop at them and new edges
// resolve to them.
#include "xdec/spec/lift.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>

#include "xdec/analysis/noreturn.h"

namespace xdec::spec {

Result<LiftedBlock> liftBasicBlock(const SpecEngine& engine,
                                   std::span<const std::byte> bytes, uint64_t base) {
  const unsigned width = engine.program().insnWidth / 8;
  if (bytes.empty() || bytes.size() % width != 0) {
    return err(DiagCode::BadFormat, "lift range must be a whole number of instructions");
  }
  const std::size_t count = bytes.size() / width;
  const uint64_t end = base + bytes.size();
  const auto inRange = [&](uint64_t va) { return va >= base && va < end; };

  auto function =
      std::make_unique<il::Function>(engine.program().arch, engine.program().registers, base);
  function->setMaturity(il::Maturity::Lifted);

  const auto insnAt = [&](std::size_t index) {
    return engine.decode(bytes.subspan(index * width, width), base + index * width);
  };

  // Pass one: where blocks begin. Internal targets lead real blocks; external
  // ones get stubs so elaboration can resolve every edge.
  std::set<uint64_t> leaders{base};
  std::set<uint64_t> external;
  for (std::size_t index = 0; index < count; ++index) {
    const InsnFlow flow = engine.probe(insnAt(index));
    if (!flow.terminates()) {
      // A fall-through instruction continues the current block; its `next` is
      // not a leader. (probe reports fallthrough for every instruction, so
      // this guard is what keeps basic blocks whole.)
      continue;
    }
    for (const uint64_t target : {flow.target, flow.fallthrough}) {
      if (target == 0) {
        continue;
      }
      (inRange(target) ? leaders : external).insert(target);
    }
    if (index + 1 < count) {
      leaders.insert(base + (index + 1) * width);
    }
  }
  // A fallthrough off the end of the range is an edge to the first unlisted
  // address.
  external.insert(end);

  std::map<uint64_t, il::BlockId> blocks;
  for (const uint64_t leader : leaders) {
    blocks.emplace(leader, function->createBlock(leader));
  }
  for (const uint64_t stub : external) {
    if (blocks.contains(stub)) {
      continue;
    }
    const il::BlockId id = function->createBlock(stub);
    function->block(id).endVa = stub;
    function->block(id).external = true;
    blocks.emplace(stub, id);
  }
  function->setEntryBlock(blocks.at(base));

  LiftSite site;
  site.function = function.get();
  site.blockAt = [&blocks](uint64_t target) {
    const auto found = blocks.find(target);
    return found == blocks.end() ? il::BlockId{} : found->second;
  };

  // Pass two: elaborate, closing each block as the next leader is reached and
  // spelling out the fallthrough edge the engine cannot see.
  auto current = blocks.find(base);
  for (std::size_t index = 0; index < count; ++index) {
    const uint64_t va = base + index * width;
    if (const auto leader = blocks.find(va);
        leader != blocks.end() && leader != current && inRange(va)) {
      function->block(current->second).endVa = va;
      const il::Block& previous = function->block(current->second);
      if (previous.ops.empty() || !function->op(previous.ops.back()).isTerminator()) {
        function->appendBranch(current->second, va - width, leader->second);
      }
      current = leader;
    }
    site.block = current->second;
    site.address = va;
    XDEC_TRY_VOID(engine.elaborate(insnAt(index), site));
  }
  function->block(current->second).endVa = end;
  {
    const il::Block& last = function->block(current->second);
    if (last.ops.empty() || !function->op(last.ops.back()).isTerminator()) {
      function->appendBranch(current->second, end - width, blocks.at(end));
    }
  }
  function->rebuildEdges();

  LiftedBlock result;
  result.function = std::move(function);
  result.block = blocks.at(base);
  return result;
}

namespace {

/// The two-phase discovery machine shared by liftFunction and
/// liftBlocksInto (see the file header for the split).
class Discovery {
 public:
  Discovery(const SpecEngine& engine, const ByteReader& reader, il::Function& function,
            const MemoryFacts& facts, const LiftCoverage* coverage, LiftRange fence,
            const TailCallTargetTest& isTailCall)
      : engine_(engine),
        reader_(reader),
        function_(function),
        facts_(facts),
        coverage_(coverage != nullptr && coverage->active() ? coverage : nullptr),
        fence_(fence),
        isTailCall_(isTailCall) {
    const SpecProgram& program = engine_.program();
    width_ = program.insnWidth / 8;

    // Pre-seed with what is already lifted: existing starts are scan
    // boundaries and edge targets, existing extents are honoured (never
    // rescanned, never split — a mid-block target of an existing block is
    // out of scope for incremental discovery by design).
    for (const il::BlockId id : function_.blockHandles()) {
      const il::Block& block = function_.block(id);
      leaders_.insert(block.va);
      blocks_.emplace(block.va, id);
      existing_.insert(block.va);
      if (!block.external) {
        extents_.emplace(block.va, block.endVa);
      }
    }
  }

  [[nodiscard]] bool valid() const { return width_ != 0 && width_ <= 16; }

  /// Phase one: map extents by probing, depth-first over direct control flow.
  void scan(std::span<const uint64_t> entries) {
    std::deque<uint64_t> worklist;
    for (const uint64_t entry : entries) {
      if (!extents_.contains(entry) && fence_.contains(entry) && readable(entry) &&
          (coverage_ == nullptr || coverage_->contains(entry))) {
        leaders_.insert(entry);
        worklist.push_back(entry);
      }
    }

    while (!worklist.empty()) {
      const uint64_t start = worklist.front();
      worklist.pop_front();
      if (extents_.contains(start) || !fence_.contains(start)) {
        continue;
      }
      tailCalls_.erase(start);

      uint64_t at = start;
      while (true) {
        // Ran into a leader discovered by an earlier scan: this block ends
        // just short of it, with a fall-through edge spelled out in phase two.
        if (at != start && leaders_.contains(at)) {
          break;
        }
        if (fence_.active() && at >= fence_.end) {
          external_.insert(at);
          break;
        }
        if (coverage_ != nullptr && !coverage_->contains(at)) {
          break;
        }
        std::array<std::byte, 16> storage{};
        if (!readWord(at, storage)) {
          // Fell off the mapped world mid-block (or the block starts there).
          unresolved_.push_back(start);
          break;
        }
        const DecodedInsn insn =
            engine_.decode(std::span<const std::byte>{storage}.first(width_), at);
        const InsnFlow flow = engine_.probe(insn);
        at += width_;
        if (flow.calls && flow.callTargetKnown &&
            analysis::isNoreturnCallTarget(flow.callTarget, reader_, facts_)) {
          // A call that is known to never return ends the block right here:
          // its fall-through is not more of this function, it is whatever
          // code happens to sit next in the image (see analysis/noreturn.h).
          // Leaving it unscanned is what a real return address would also
          // leave unscanned; nothing downstream needs to know why.
          break;
        }
        if (!flow.terminates()) {
          continue;
        }
        std::vector<uint64_t> targets;
        if (coverage_ != nullptr) {
          targets = coverage_->successors(at - width_);
          // Elaboration still needs handles for both statically encoded arms.
          // Arms not taken by the trace are boundary stubs and are removed
          // from the terminator immediately after elaboration.
          for (const uint64_t encoded : {flow.target, flow.fallthrough}) {
            if (encoded != 0 &&
                std::ranges::find(targets, encoded) == targets.end()) {
              external_.insert(encoded);
            }
          }
        } else {
          if (flow.kind == FlowKind::Branch && flow.target != 0 &&
              isTailCall_ && !extents_.contains(flow.target) &&
              isTailCall_(at - width_, flow.target)) {
            tailCalls_[start] = flow.target;
            break;
          }
          if (flow.kind == FlowKind::IndirectBranch || flow.kind == FlowKind::Unknown) {
            unresolved_.push_back(start);
          }
          targets = {flow.target, flow.fallthrough};
        }
        for (const uint64_t target : targets) {
          if (target == 0) {
            continue;
          }
          // Only mapped targets inside the active fence become real blocks;
          // the rest get a stub so the branch still resolves to a BlockId.
          if (!fence_.contains(target) || !readable(target)) {
            external_.insert(target);
            continue;
          }
          if (leaders_.insert(target).second) {
            worklist.push_back(target);
          }
          // A target landing strictly inside an already-scanned extent splits
          // it: drop the coarse extent and rescan from its start against the
          // finer leader set. Converges because leaders only ever grow.
          // Existing blocks are exempt (see the constructor's comment).
          const auto after = extents_.upper_bound(target);
          if (after != extents_.begin()) {
            const auto before = std::prev(after);
            if (before->first < target && target < before->second &&
                !existing_.contains(before->first)) {
              worklist.push_back(before->first);
              extents_.erase(before);
            }
          }
        }
        break;
      }
      extents_[start] = at;
    }
  }

  /// Phase two: create blocks for the new extents and elaborate them.
  Result<std::vector<uint64_t>> elaborate() {
    std::vector<uint64_t> lifted;
    for (const auto& [start, end] : extents_) {
      if (existing_.contains(start)) {
        continue;  // already has a block with real content
      }
      lifted.push_back(start);
      blocks_.emplace(start, function_.createBlock(start));
      existing_.insert(start);  // one block per extent, even on a re-run
    }
    for (const uint64_t stub : external_) {
      if (blocks_.contains(stub)) {
        continue;
      }
      const il::BlockId id = function_.createBlock(stub);
      function_.block(id).endVa = stub;
      function_.block(id).external = true;
      blocks_.emplace(stub, id);
    }

    LiftSite site;
    site.function = &function_;
    site.blockAt = [this](uint64_t target) {
      const auto found = blocks_.find(target);
      return found == blocks_.end() ? il::BlockId{} : found->second;
    };

    for (const uint64_t start : lifted) {
      const uint64_t end = extents_.at(start);
      const il::BlockId block = blocks_.at(start);
      const auto tailIt = tailCalls_.find(start);
      const uint64_t liftEnd =
          (tailIt != tailCalls_.end() && end >= start + width_) ? end - width_ : end;
      site.block = block;
      for (uint64_t at = start; at < liftEnd; at += width_) {
        std::array<std::byte, 16> storage{};
        if (!readWord(at, storage)) {
          break;  // mapped in phase one; if it vanished, end the block here
        }
        site.address = at;
        XDEC_TRY_VOID(engine_.elaborate(
            engine_.decode(std::span<const std::byte>{storage}.first(width_), at), site));
      }
      function_.block(block).endVa = end;
      if (tailIt != tailCalls_.end()) {
        const uint64_t tailVa = end >= width_ ? end - width_ : end;
        (void)function_.appendCall(
            block, tailVa, function_.constant(il::Type::integer(64), tailIt->second),
            il::Type::integer(64));
        (void)function_.appendReturn(block, tailVa);
      }
      const il::Block& built = function_.block(block);
      if (built.ops.empty() || !function_.op(built.ops.back()).isTerminator()) {
        const il::BlockId next = site.blockAt(end);
        const uint64_t attributedTo = end >= width_ ? end - width_ : end;
        if (next.valid() && next != block) {
          // Attributed to the last instruction, like the fall-through edges
          // the engine itself emits.
          function_.appendBranch(block, attributedTo, next);
        } else {
          // No block picks up where this one stopped: either the scan ended
          // it on a known-noreturn call (see the check in scan() above) or it
          // ran off mapped memory mid-block. Either way nothing legitimately
          // follows, so the block gets a real terminator instead of silently
          // trailing off without one.
          function_.appendUnreachable(block, attributedTo);
        }
      }
      if (coverage_ != nullptr) {
        il::Block& observed = function_.block(block);
        if (!observed.ops.empty()) {
          const il::OpId terminatorId = observed.ops.back();
          il::Op& terminator = function_.op(terminatorId);
          if (terminator.isTerminator() &&
              (terminator.code == il::OpCode::Branch ||
               terminator.code == il::OpCode::CondBranch ||
               terminator.code == il::OpCode::IndirectBranch)) {
            const std::vector<uint64_t> successors =
                coverage_->successors(terminator.va);
            std::vector<il::BlockId> targets;
            targets.reserve(successors.size());
            for (const uint64_t successor : successors) {
              const il::BlockId target = site.blockAt(successor);
              if (target.valid()) {
                targets.push_back(target);
              }
            }
            if (targets.size() == 1) {
              terminator.code = il::OpCode::Branch;
              function_.setOperands(terminatorId, {});
              function_.setTargets(terminatorId, targets);
            } else if (!targets.empty() &&
                       terminator.code == il::OpCode::IndirectBranch) {
              function_.setTargets(terminatorId, targets);
            }
          }
        }
      }
    }
    function_.rebuildEdges();
    return lifted;
  }

  [[nodiscard]] il::BlockId blockAt(uint64_t va) const {
    const auto found = blocks_.find(va);
    return found == blocks_.end() ? il::BlockId{} : found->second;
  }
  [[nodiscard]] const std::vector<uint64_t>& unresolved() const { return unresolved_; }
  [[nodiscard]] const std::set<uint64_t>& external() const { return external_; }

 private:
  [[nodiscard]] bool readable(uint64_t va) const {
    std::array<std::byte, 16> probe{};
    return readWord(va, probe).hasValue();
  }
  [[nodiscard]] Result<void> readWord(uint64_t va, std::span<std::byte, 16> storage) const {
    return reader_(va, storage.first(width_));
  }

  const SpecEngine& engine_;
  const ByteReader& reader_;
  il::Function& function_;
  const MemoryFacts& facts_;
  const LiftCoverage* coverage_ = nullptr;
  LiftRange fence_{};
  const TailCallTargetTest& isTailCall_;
  unsigned width_ = 0;

  std::set<uint64_t> leaders_;
  std::set<uint64_t> external_;
  std::map<uint64_t, uint64_t> tailCalls_;
  std::map<uint64_t, uint64_t> extents_;  // block start -> just past its end
  std::map<uint64_t, il::BlockId> blocks_;
  std::set<uint64_t> existing_;           // starts that already have content
  std::vector<uint64_t> unresolved_;
};

}  // namespace

Result<LiftedFunction> liftFunction(const SpecEngine& engine, const ByteReader& reader,
                                    uint64_t entry, const MemoryFacts& facts,
                                    const LiftCoverage* coverage, LiftRange fence,
                                    const TailCallTargetTest& isTailCall) {
  auto function =
      std::make_unique<il::Function>(engine.program().arch, engine.program().registers, entry);
  function->setMaturity(il::Maturity::Lifted);

  Discovery discovery(engine, reader, *function, facts, coverage, fence, isTailCall);
  if (!discovery.valid()) {
    return err(DiagCode::Internal, "unsupported instruction width");
  }
  discovery.scan(std::span<const uint64_t>{&entry, 1});
  XDEC_TRY(auto lifted, discovery.elaborate());
  (void)lifted;

  const il::BlockId entryBlock = discovery.blockAt(entry);
  if (!entryBlock.valid()) {
    return err(DiagCode::LiftFailure, "entry address is not mapped");
  }
  function->setEntryBlock(entryBlock);

  LiftedFunction result;
  result.function = std::move(function);
  result.entry = entryBlock;
  result.unresolved = discovery.unresolved();
  result.external.assign(discovery.external().begin(), discovery.external().end());
  return result;
}

Result<LiftBlocksResult> liftBlocksInto(il::Function& function, const SpecEngine& engine,
                                        const ByteReader& reader,
                                        std::span<const uint64_t> entries,
                                        const MemoryFacts& facts,
                                        const LiftCoverage* coverage, LiftRange fence,
                                        const TailCallTargetTest& isTailCall) {
  Discovery discovery(engine, reader, function, facts, coverage, fence, isTailCall);
  if (!discovery.valid()) {
    return err(DiagCode::Internal, "unsupported instruction width");
  }
  discovery.scan(entries);
  LiftBlocksResult result;
  XDEC_TRY(result.lifted, discovery.elaborate());
  result.unresolved = discovery.unresolved();
  return result;
}

}  // namespace xdec::spec
