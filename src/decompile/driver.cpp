// The decompilation driver (see the header for the loop and why re-lifting
// beats stitching).
#include "xdec/decompile/driver.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xdec/analysis/reachability.h"
#include "xdec/passes/resolve_indirect.h"
#include "xdec/spec/lift.h"
#include "xdec/support/log.h"

namespace xdec::decompile {

// Round-by-round progress. Discovery multiplies work: a round that finds a
// 1351-entry dispatcher hands the next round 1351 more entries to lift, so the
// rounds are not equal in cost and a run can take minutes with nothing to show
// for the wait. Set XDEC_LOG=driver=info.
XDEC_DEFINE_LOG_CATEGORY(driverLog, "driver")

namespace {

/// Everything about the address space a pipeline gets, in one place: every
/// round builds its own Manager, and a fact wired into one round but not
/// another would make the rounds disagree about the same function.
void configure(pass::Manager& manager, const ByteReader& reader,
               const DriverOptions& options) {
  manager.setImage(reader);
  manager.setMemoryFacts(options.memory);
  manager.setTypeDatabase(options.types);
  manager.setSyscallTable(options.syscalls);
  manager.setNames(options.names);
  manager.setEntryRegFacts(options.entryRegs);
  // Inert below Resolved, which is as far as every round but the last goes, so
  // it belongs here with the rest rather than only on the finishing Manager.
  manager.setSealUnresolvedBranches(options.sealUnresolvedBranches);
}

/// Which targets an indirect branch was proved to have, keyed by the branch's
/// own address so it survives the next round's re-lift (block handles do not).
using Resolutions = std::map<uint64_t, std::vector<uint64_t>>;

/// Harvests every resolved indirect branch, for the next round to re-apply.
[[nodiscard]] Resolutions harvest(const il::Function& function) {
  Resolutions out;
  for (const il::BlockId blockId : function.blockHandles()) {
    const il::Block& block = function.block(blockId);
    if (block.ops.empty()) {
      continue;
    }
    const il::Op& terminator = function.op(block.ops.back());
    const auto targets = function.targets(terminator);
    if (terminator.code != il::OpCode::IndirectBranch || targets.empty()) {
      continue;
    }
    std::vector<uint64_t> addresses;
    addresses.reserve(targets.size());
    for (const il::BlockId target : targets) {
      addresses.push_back(function.block(target).va);
    }
    out.emplace(terminator.va, std::move(addresses));
  }
  return out;
}

/// Merges what a round proved into what is already known, and says whether any
/// of it was new.
///
/// Targets accumulate rather than replace. A round proves edges; it never
/// disproves them, so a round whose analysis happens to see fewer of them than
/// the last must not take away what the last one established. That is also what
/// makes the loop finish: the known set only ever grows, and there are finitely
/// many edges to find. Replacing the set and asking whether it differs from the
/// previous one instead treats a branch whose target list merely reorders — or
/// gains a target one round and loses it the next — as progress, and the loop
/// then spends its whole budget re-lifting a function it already understood.
[[nodiscard]] std::size_t absorb(Resolutions& known, const Resolutions& found) {
  std::size_t added = 0;
  for (const auto& [branch, targets] : found) {
    std::vector<uint64_t>& mine = known[branch];
    for (const uint64_t target : targets) {
      if (std::find(mine.begin(), mine.end(), target) == mine.end()) {
        mine.push_back(target);
        ++added;
      }
    }
  }
  return added;
}

/// Re-establishes known edges on a freshly lifted CFG, so that what a previous
/// round proved is part of the graph before anything reads dataflow from it.
///
/// Only for branches still unresolved and only for targets that are now block
/// starts: a resolution whose target this round's lifting placed differently is
/// dropped rather than forced, because the address is the claim and a block
/// boundary that moved means the claim was about something else.
void applyResolutions(il::Function& function, const Resolutions& known) {
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
    const auto found = known.find(terminator.va);
    if (found == known.end()) {
      continue;
    }
    std::vector<il::BlockId> targets;
    targets.reserve(found->second.size());
    for (const uint64_t va : found->second) {
      const il::BlockId target = function.blockAt(va);
      if (!target.valid()) {
        targets.clear();
        break;
      }
      targets.push_back(target);
    }
    if (targets.empty()) {
      continue;
    }
    function.setTargets(terminatorId, targets);
    changed = true;
  }
  if (changed) {
    function.rebuildEdges();
  }
}

/// One intermediate resolution probe: runs the (already verified-at-Ssa)
/// function through resolve-indirect outside the Manager, collecting the
/// block addresses it discovers. The final, fully verified Manager run is
/// the gate; this probe only decides whether more lifting is worthwhile.
struct Probe {
  /// Addresses with no block yet: what the next round lifts.
  std::set<uint64_t> discoveries;
  /// The edges those addresses belong to, so the next round can put them back
  /// before it builds SSA over the blocks it is about to lift.
  Resolutions pending;
  /// Branches whose candidate set was too wide to believe (see
  /// DriverOptions::maxDiscoveryPerBranch), counted so the round can say so.
  std::size_t declined = 0;

  Result<void> run(il::Function& function, const ByteReader& reader,
                   const DriverOptions& options) {
    std::unique_ptr<pass::Pass> pass = passes::makeResolveIndirectPass();
    pass::Context context(function);
    context.setImage(reader);
    // The same address-space facts every Manager round gets, for the same
    // reason: a probe that knows less than the run it is predicting predicts
    // the wrong thing. Sealing is deliberately not among them: it is what to do
    // once discovery is over, and doing it here would destroy the branches the
    // remaining rounds exist to resolve.
    context.setMemoryFacts(options.memory);
    context.setEntryRegFacts(options.entryRegs);
    context.setDiscoverySink([this, &options](const pass::Discovery& found) {
      if (options.maxDiscoveryPerBranch != 0 &&
          found.missing.size() > options.maxDiscoveryPerBranch) {
        // See DriverOptions::maxDiscoveryPerBranch. Neither the addresses nor
        // the edge is taken: the candidate set is what would have been put
        // back before SSA, and putting back a set this pass is being told not
        // to believe would open exactly the edges the cap exists to refuse.
        ++declined;
        XDEC_LOG_DEBUG(driverLog(),
                       "branch {:#x} offers {} unlifted target(s), past the {}-per-branch "
                       "cap; none are taken and the branch is left unresolved",
                       found.branch, found.missing.size(), options.maxDiscoveryPerBranch);
        return;
      }
      discoveries.insert(found.missing.begin(), found.missing.end());
      pending[found.branch] = found.targets;
    });
    XDEC_TRY_VOID(pass->run(context));
    return {};
  }
};

/// The wall behind the intra-round settle loop (see its call site): however
/// deep a single round's own discoveries chain into each other, a round
/// only ever gets this many extra lift-and-probe passes before it yields to
/// the outer round accounting, the same "structural argument deserves a hard
/// backstop anyway" reasoning kRoundCeiling exists for.
constexpr unsigned kSettleCeiling = 16;

}  // namespace

Result<DriverResult> decompile(const spec::SpecEngine& engine, const ByteReader& reader,
                               uint64_t entry, pass::Registry& registry,
                               const DriverOptions& options) {
  DriverReport report;

  // Every entry a round discovered, in first-seen order; the next round
  // re-lifts from all of them.
  std::vector<uint64_t> entries{entry};
  std::set<uint64_t> known{entry};
  // What the rounds proved about the branches leading to them.
  Resolutions resolved;

  // Admits a probe's discoveries into `entries`/`known` (fence and
  // total-entry cap applied exactly as a single-pass round always has),
  // logging the same shape-of-discovery warnings a round has always logged.
  // Factored out because the intra-round settle loop below calls this once
  // per internal pass, not just once per round: a chain of discoveries a
  // round finds inside itself is admitted the same way regardless of which
  // pass within the round found it.
  const auto admitDiscoveries = [&](const std::set<uint64_t>& discoveries) -> std::size_t {
    std::size_t foundNew = 0;
    std::size_t outsideFence = 0;
    std::size_t capped = 0;
    for (const uint64_t va : discoveries) {
      if (options.fence.active() && !options.fence.contains(va)) {
        ++outsideFence;
        // Advisory unless the caller asked otherwise (see FunctionFence), and
        // deliberately quiet about it: a linker-recorded size that undershoots
        // by a few bytes -- a jump table placed right past the code the size
        // accounts for, say -- is routine and not a sign of anything wrong, so
        // this is a debug trace for a real bleed-through investigation to pull
        // up, not a warning every ordinary run would print.
        XDEC_LOG_DEBUG(driverLog(),
                       "discovery {:#x} is outside the entry's extent [{:#x}, {:#x}); {}",
                       va, options.fence.start, options.fence.end,
                       options.fence.enforce ? "dropping it" : "lifting it anyway");
        if (options.fence.enforce) {
          continue;
        }
      }
      if (known.contains(va)) {
        continue;
      }
      // The hard backstop (see DriverOptions::maxTotalEntries): everything
      // admitted before the cap still gets lifted and emitted, same as a
      // round-cap cutoff, so this is one more way the run ends up partial
      // rather than one more way it fails outright.
      if (known.size() >= options.maxTotalEntries) {
        ++capped;
        continue;
      }
      known.insert(va);
      entries.push_back(va);
      report.extraEntries.push_back(va);
      ++foundNew;
    }
    if (capped > 0) {
      XDEC_LOG_WARN(driverLog(),
                    "round {} hit the {}-entry hard cap with {} more address(es) "
                    "discovered but not admitted; if the entry is a real function "
                    "start this should not happen, so the result from here on is "
                    "worth treating as partial",
                    report.rounds, options.maxTotalEntries, capped);
    }
    // A few stray addresses past an undershooting symbol size are routine
    // (see the debug trace above); hundreds are not. That shape is exactly
    // what an unbounded jump-table enumeration off an entry that turns out
    // not to be a real function start looks like -- the case this exists to
    // catch is sub_627ac-in-bc_lib, where treating a mid-MBA address as an
    // entry (no fence at all, since it has no symbol) let one round's
    // discovery pull in 1349 jump-table targets and inflate the output to
    // tens of thousands of lines with nothing louder than an INFO line to
    // notice it by.
    constexpr std::size_t kLargeDiscoveryWarning = 512;
    constexpr std::size_t kLargeFencedOverrunWarning = 32;
    if (!options.fence.active() && foundNew > kLargeDiscoveryWarning) {
      XDEC_LOG_WARN(driverLog(),
                    "round {} discovered {} new address(es) with no function-size fence "
                    "active; if the entry is not a real function start, this is likely a "
                    "jump table being enumerated as if it were free-standing code rather "
                    "than bounded by a caller's function (pass a symbol with a size, or "
                    "double-check the entry address)",
                    report.rounds, foundNew);
    } else if (options.fence.active() && options.fence.enforce && outsideFence > 0) {
      // Not a warning: the caller set an enforcing fence precisely so this
      // would happen, and the output being partial is the bargain it made.
      // Still worth a line, because "partial" is only useful if it is visible.
      XDEC_LOG_INFO(driverLog(),
                    "round {} dropped {} discovered address(es) outside the requested "
                    "extent [{:#x}, {:#x}); the function is emitted without them",
                    report.rounds, outsideFence, options.fence.start, options.fence.end);
    } else if (options.fence.active() && outsideFence > kLargeFencedOverrunWarning) {
      XDEC_LOG_WARN(driverLog(),
                    "round {} discovered {} address(es) outside the entry's symbol extent "
                    "[{:#x}, {:#x}); this is too many to be the usual few-byte undershoot, "
                    "and likely means the fence itself is wrong for this function",
                    report.rounds, outsideFence, options.fence.start, options.fence.end);
    }
    return foundNew;
  };

  // Whether some resolved branch's own target address is stuck: fully known
  // (admitted, not merely awaiting a fence or cap decision) yet still not a
  // real block in the freshly lifted function -- unmapped memory, usually.
  // Distinct from "not yet lifted": the settle loop above already lifts
  // everything admission accepts, so if a known target still has no block
  // here, no further pass or round will change that, and a round declaring
  // convergence at this point would be reporting a fixpoint that quietly
  // leaves this branch's edge out of it.
  const auto hasStuckResolution = [&](const il::Function& function) -> bool {
    for (const auto& [branch, targets] : resolved) {
      for (const uint64_t target : targets) {
        if (known.contains(target) && !function.blockAt(target).valid()) {
          return true;
        }
      }
    }
    return false;
  };

  // Fresh-lifts the whole function from `entry` plus every entry known so
  // far -- the same "one consistent maturity, never a quilt" lift every
  // round already did, now also reused mid-round by the settle loop to bring
  // a pass's own just-admitted discoveries in as real blocks.
  const auto liftKnown = [&]() -> Result<spec::LiftedFunction> {
    auto lifted = spec::liftFunction(engine, reader, entry, options.memory);
    if (!lifted) {
      return std::move(lifted).takeUnexpected();
    }
    if (entries.size() > 1) {
      const std::vector<uint64_t> extra(entries.begin() + 1, entries.end());
      auto more = spec::liftBlocksInto(*lifted->function, engine, reader, extra, options.memory);
      if (!more) {
        return std::move(more).takeUnexpected();
      }
    }
    return lifted;
  };

  // The one run whose result is returned: the whole pipeline, observed so dumps
  // land, over a fresh lift of every entry with every proved edge already in
  // place. Both ways out of the loop end here, so a run cut short by the cap
  // still yields the function the rounds did manage to uncover.
  auto finish = [&]() -> Result<DriverResult> {
    auto whole = spec::liftFunction(engine, reader, entry, options.memory);
    if (!whole) {
      return std::move(whole).takeUnexpected();
    }
    if (entries.size() > 1) {
      const std::vector<uint64_t> extra(entries.begin() + 1, entries.end());
      auto more = spec::liftBlocksInto(*whole->function, engine, reader, extra, options.memory);
      if (!more) {
        return std::move(more).takeUnexpected();
      }
    }
    pass::Manager manager(options.observer);
    configure(manager, reader, options);
    XDEC_TRY_VOID(manager.runTo(*whole->function, registry, il::Maturity::Cfg));
    applyResolutions(*whole->function, resolved);
    XDEC_TRY_VOID(manager.runTo(*whole->function, registry, options.target));
    // The second half of the jump-table candidate defence (see
    // analysis/reachability.h): every block the candidate filtering and the
    // discovery loop above actually needed should be reachable from the
    // entry by now. One that is not was lifted for nothing -- silently
    // harmless, since nothing downstream emits it, but worth surfacing rather
    // than leaving to be noticed as an unexplained unused local.
    const std::unordered_set<il::BlockId> reachable =
        analysis::reachableBlocks(*whole->function);
    if (reachable.size() < whole->function->blockCount()) {
      report.unreachableBlocks = whole->function->blockCount() - reachable.size();
      XDEC_LOG_WARN(driverLog(),
                    "{} block(s) in the lifted function have no path from entry; nothing "
                    "emits them, but a resolve or discovery step claimed one that turns "
                    "out not to be reachable",
                    report.unreachableBlocks);
    }
    return DriverResult{std::move(whole->function), std::move(report)};
  };

  // Grows a round at a time while rounds keep proving something; see
  // DriverOptions::extendWhileProving.
  unsigned budget = std::min(options.maxRounds, DriverOptions::kRoundCeiling);

  for (unsigned round = 0; round < budget; ++round) {
    // Fresh and consistent: the whole function at Lifted, every round.
    auto lifted = liftKnown();
    if (!lifted) {
      return std::move(lifted).takeUnexpected();
    }
    ++report.rounds;

    // Targets at or below Ssa need no discovery loop: run once, observed.
    if (options.target <= il::Maturity::Ssa) {
      pass::Manager manager(options.observer);
      configure(manager, reader, options);
      XDEC_TRY_VOID(manager.runTo(*lifted->function, registry, options.target));
      return DriverResult{std::move(lifted->function), std::move(report)};
    }

    // A round settles to its own fixed point before the outer loop asks
    // whether *it* found anything. Resolve-indirect shaping a branch's full
    // candidate set is not the same as those candidates being blocks: the
    // first one is a pass; the second needs a re-lift. Without this, a chain
    // of discoveries nested inside a single round -- one target's own
    // indirect branch only shapeable once *it* has a block -- cost one
    // *round* of the outer budget per link, which is what left absd's 9a4
    // still sealed at a 4-round budget despite resolve-indirect having
    // already shaped all three of its targets by then. See the header for
    // why one round is supposed to open one level of a dispatcher chain;
    // this loop is what makes that true when a level itself takes more than
    // one probe to see.
    std::size_t foundNew = 0;
    std::size_t foundEdges = 0;
    for (unsigned settle = 0; settle < kSettleCeiling; ++settle) {
      {
        pass::Manager manager;  // intermediate passes run unobserved
        configure(manager, reader, options);
        // Stop at Cfg to put the last pass's edges back before SSA is built
        // over them; see the header for why doing it after would be too late.
        XDEC_TRY_VOID(manager.runTo(*lifted->function, registry, il::Maturity::Cfg));
        applyResolutions(*lifted->function, resolved);
        XDEC_TRY_VOID(manager.runTo(*lifted->function, registry, il::Maturity::Ssa));
      }

      Probe probe;
      XDEC_TRY_VOID(probe.run(*lifted->function, reader, options));

      const std::size_t liftedFrom = entries.size();  // before this pass's finds
      const std::size_t passNew = admitDiscoveries(probe.discoveries);
      foundNew += passNew;
      if (probe.declined > 0) {
        XDEC_LOG_INFO(driverLog(),
                      "round {} left {} branch(es) unresolved whose candidate sets were "
                      "wider than the {}-per-branch discovery cap",
                      report.rounds, probe.declined, options.maxDiscoveryPerBranch);
      }
      // Resolving a branch is progress even when it revealed no new address:
      // the edge it adds is what lets the next pass's SSA reach one level
      // further in, and stopping here would leave that level unanalysed.
      // Measured per edge, not per branch, because a dispatcher's target set
      // widens a few entries at a time while the count of branches holding
      // one stands still.
      const Resolutions found = harvest(*lifted->function);
      std::size_t passEdges = absorb(resolved, found);
      // Edges of branches that could not resolve only because their targets
      // had no blocks yet. Carrying them is what makes a chain of them cost
      // one pass per level instead of two: the pass that lifts the block
      // also gets its incoming edge, so SSA sees a reachable block and the
      // branch *it* ends with resolves in that same pass (see the header).
      passEdges += absorb(resolved, probe.pending);
      foundEdges += passEdges;
      XDEC_LOG_INFO(driverLog(),
                    "round {} pass {}: {} block(s) from {} entr(ies), {} branch(es) "
                    "resolved, {} new address(es), {} new edge(s)",
                    report.rounds, settle + 1, lifted->function->blockCount(), liftedFrom,
                    found.size(), passNew, passEdges);
      if (passNew == 0) {
        // Nothing this pass admitted needs lifting; another pass over the
        // same blocks would just re-derive what this one already proved.
        break;
      }
      // The pass's own discoveries are only candidates until they are real
      // blocks; lift them in before asking resolve-indirect about them again.
      auto relifted = liftKnown();
      if (!relifted) {
        return std::move(relifted).takeUnexpected();
      }
      lifted = std::move(relifted);
    }

    if (foundNew == 0 && foundEdges == 0) {
      if (hasStuckResolution(*lifted->function)) {
        // See hasStuckResolution: a real, permanent gap, not a convergence.
        // The function is still emitted as far as the rounds got it, same as
        // a round-cap stop, but report.converged stays false so the report
        // does not claim a fixpoint this run never actually reached.
        XDEC_LOG_WARN(driverLog(),
                      "round {}: a resolved branch's target never became a reachable "
                      "block (likely unmapped memory); stopping here rather than "
                      "reporting a convergence that leaves it out",
                      report.rounds);
        return finish();
      }
      report.converged = true;
      return finish();
    }
    // Reaching here means the round proved something, so the budget running out
    // now would cut the loop short mid-discovery rather than at a fixpoint.
    if (round + 1 == budget && options.extendWhileProving &&
        budget < DriverOptions::kRoundCeiling) {
      ++budget;
      XDEC_LOG_INFO(driverLog(),
                    "round {} still proved something at the {}-round budget; granting "
                    "another (ceiling {})",
                    report.rounds, options.maxRounds, DriverOptions::kRoundCeiling);
    }
  }

  // The cap, not convergence. Everything proved so far still gets emitted; see
  // the header for why that beats reporting failure and discarding it.
  XDEC_LOG_WARN(driverLog(),
                "stopped at the {}-round cap with {} extra entr(ies) still settling "
                "(last at {:#x}); the function is emitted as far as the rounds got it",
                budget, report.extraEntries.size(),
                report.extraEntries.empty() ? 0 : report.extraEntries.back());
  return finish();
}

}  // namespace xdec::decompile
