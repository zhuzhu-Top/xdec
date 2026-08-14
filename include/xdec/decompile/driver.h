// The decompilation driver: discovery and transformation as one loop.
//
// Neither the lifter nor the pass pipeline can finish the job alone:
//
//   - The lifter discovers blocks by following control flow it understands.
//     Indirect branches it cannot follow, so their targets stay dark.
//   - The pipeline understands indirect branches — after SSA and
//     simplification, a `br x8` reads as "one of these table entries" — but
//     by then the blocks behind it were never lifted, and the pipeline does
//     not lift.
//
// The driver closes the loop. Each round it re-lifts the function fresh —
// from the entry plus every address discovered so far — so the IL the
// pipeline sees is always one consistent maturity, never a quilt of new
// Lifted blocks stitched into Ssa fabric (maturity is a ratchet; mixed
// functions fail verification, and stitching register versions across the
// seam would be worse than failing). It then runs the verified pipeline to
// Ssa and probes resolution: every indirect target that still lacks a block
// is a discovery for the next round. When a round discovers nothing, the
// final run goes to the requested target under full verification — the gate
// is exactly as strict as if no loop existed.
//
// Rounds carry their resolutions forward, and this is what makes the loop
// converge rather than merely repeat. A block lifted because a dispatcher
// pointed at it is, on the next round, still a block nothing branches to: the
// edge only appears when resolution runs, and resolution runs after SSA
// construction. So without the carry, every discovered block is unreachable
// exactly when SSA is built, register reads inside it never become values, and
// the dispatcher *it* ends with is then unresolvable in principle — its target
// is an expression over registers no dataflow reaches. A dispatcher of
// dispatchers, which is what a flattened function is, would stall one level in.
// Re-applying the previous round's resolutions right after the CFG is built
// closes that gap: SSA sees the graph resolution already proved, and each round
// reaches one level deeper.
//
// What is carried is the *candidate set*, not only the resolved edge, and the
// difference is a factor of two on a chain. A branch whose targets are proved
// but not yet lifted does not resolve — the blocks are not there to point at —
// so if only resolved edges were carried, the round that lifts those blocks
// would find the branch unresolved again, resolve it then, and only the round
// after that would build SSA over the edge. The block would spend the round it
// was lifted in unreachable, exactly the situation above. Reporting the whole
// candidate set with the discovery (see pass::Discovery) means the round that
// lifts a level also opens it: one round per level of a dispatcher chain
// instead of two, which is what brings an obfuscated JNI_OnLoad inside the
// default cap.
//
// Convergence is structural: each round only adds addresses already in the
// image, and the address space is finite. The round cap exists because
// "structural" arguments deserve a hard backstop anyway.
//
// Hitting that cap is not an error. On a heavily obfuscated function the rounds
// widen a dispatcher's target set a few entries at a time, and the eighth round
// can still be learning something; what it has learned by then is most of the
// function, and throwing it away to report failure serves nobody. So the cap
// ends the loop the same way convergence does — the final run happens over
// everything proved so far — and `DriverReport::converged` says which of the two
// it was, for a caller that wants to warn that the picture may be incomplete.
//
// A round that is still proving edges when the budget runs out extends it
// instead (DriverOptions::extendWhileProving), up to a ceiling. The budget
// guesses how deep the function goes, and being one level out is the common
// way to be wrong; the ceiling is what the "hard backstop" argument above
// actually wants, since a loop that keeps proving new edges is by definition
// not spinning.
#pragma once

#include <memory>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/il/maturity.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/spec/engine.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::decompile {

/// A function's known extent, from its ELF symbol size when the entry point
/// has one. Advisory by default (see `enforce`): a discovery outside it is
/// logged, not dropped, because a symbol's recorded size is not always
/// trustworthy enough to bet a correct decompilation on -- stripped or
/// hand-written tables can legitimately reach past it, and refusing to lift an
/// address the code genuinely branches to would trade a bleed-through bug for
/// a missing-block one. What it is good for that way is exactly what the
/// noreturn and jump-table fixes were found by: a number in a log line for a
/// case that looks too big, instead of nothing until someone reads the output
/// by hand.
struct FunctionFence {
  uint64_t start = 0;
  uint64_t end = 0;  // exclusive; start == end means "no fence"
  /// Whether an address outside the extent is *dropped* rather than logged.
  ///
  /// The advisory reading above assumes the fence is a symbol's recorded size,
  /// which might undershoot the real function. A caller that set the fence to
  /// bound its own work rather than to describe a symbol means the opposite:
  /// on a flattened binary whose dispatch tables are shared image-wide,
  /// following every discovery is not "finding the rest of this function", it
  /// is decompiling the program -- absd's `start()` reaches tens of thousands
  /// of lines that way, none of them the entry anyone asked about. Such a
  /// caller says so here, and accepts the missing blocks that come with it:
  /// output is partial by construction, and honest about it, rather than
  /// complete and unreadable.
  bool enforce = false;

  [[nodiscard]] bool active() const noexcept { return end > start; }
  [[nodiscard]] bool contains(uint64_t va) const noexcept {
    return !active() || (va >= start && va < end);
  }
};

struct DriverOptions {
  il::Maturity target = il::Maturity::Resolved;
  pass::Observer* observer = nullptr;  // final-round dumps, if wanted
  /// Round budget (see the header).
  unsigned maxRounds = 8;
  /// Whether spending the last of the budget on a round that proved something
  /// buys another one, up to kRoundCeiling.
  ///
  /// The budget is a guess at how deep a function goes, and a dispatcher chain
  /// one level deeper than the guess is not a reason to hand back a function
  /// that was still opening up — especially since the rounds that matter are
  /// exactly the ones that keep proving edges. A caller who chose `maxRounds`
  /// to bound its own runtime means it as a wall and sets this false; the CLI
  /// does that when `--rounds` is given explicitly.
  bool extendWhileProving = true;
  /// The wall behind the budget: pathological input gets bounded work whatever
  /// it keeps claiming to prove.
  static constexpr unsigned kRoundCeiling = 128;
  /// Emit a function whose remaining computed branches could not be resolved,
  /// with each one marked, rather than failing the run (see
  /// pass::Context::setSealUnresolvedBranches).
  bool sealUnresolvedBranches = false;
  /// What is known about the image beyond its bytes — immutable ranges, code
  /// ranges — for the passes that fold or resolve on the strength of it (see
  /// pass::Context::setMemoryFacts). Absent claims nothing, which costs
  /// simplification and risks nothing.
  MemoryFacts memory;
  /// Knowledge from outside the image, for the passes that can use it: types
  /// imported from headers, and the kernel's syscall numbering. Both may be
  /// null, and both must outlive the call.
  const types::TypeDatabase* types = nullptr;
  const types::SyscallTable* syscalls = nullptr;
  /// The image's symbol table, which is what connects an imported declaration
  /// to an address (see pass::Context::setNames). Unset binds nothing.
  pass::NameAt names;
  /// The entry's own symbol extent, if the caller found one worth recording
  /// (see FunctionFence). Inactive by default: nothing about this loop needs
  /// a fence to behave, it is purely a diagnostic the caller opts into.
  FunctionFence fence;
  /// The hard backstop behind the per-round discovery-size warnings (see
  /// driver.cpp): however many addresses a round keeps discovering, the total
  /// entries a run will ever add is capped here. A real dispatcher chain
  /// widens by a handful of entries a round (single digits, in every sample
  /// this codebase ships); an entry that is not really a function start
  /// turning a jump table into free-standing "code" is the shape that hands
  /// over hundreds to thousands at once instead (the motivating case handed
  /// over 1349 in its very first round). Sized to the same order of
  /// magnitude as driver.cpp's own "this many at once is not an ordinary
  /// dispatcher" warning threshold, since that is the same judgment call
  /// this makes consequential instead of merely loud — a wall well above any
  /// legitimate count on purpose, the same role kRoundCeiling plays for
  /// rounds, for entries instead.
  std::size_t maxTotalEntries = 2048;
  /// How many not-yet-lifted targets a *single* branch may contribute in one
  /// round, or zero for no limit. Zero by default, and that is not timidity:
  /// a real table of 1351 entries is in this repository's own sample set, and
  /// a cap low enough to be useful against a bad enumeration would throw that
  /// one away.
  ///
  /// What it is for is the opposite case, where the count itself is the
  /// evidence. A dispatch in a flattened function selects among that
  /// function's states -- a handful, or a few dozen -- so a branch offering
  /// three hundred targets off a table the whole binary shares (absd's
  /// `0x100080ec0`) has not found three hundred states; it has run off the end
  /// of its own entries into the next function's, and lifting them buries the
  /// function that was asked for. A caller who knows it is looking at that
  /// shape says so with a number here.
  ///
  /// Over the cap the branch contributes *nothing*, rather than the first N.
  /// A prefix would be the worst of both: the branch still has targets
  /// missing, so it still never resolves, and the blocks lifted for the prefix
  /// are there anyway.
  std::size_t maxDiscoveryPerBranch = 0;
};

/// What the loop did, for CLI reporting and tests.
struct DriverReport {
  unsigned rounds = 0;
  /// Entries beyond the function's own that rounds lifted from, in discovery
  /// order (each round's discoveries, accumulated).
  std::vector<uint64_t> extraEntries;
  /// Whether a round finally found nothing left to find, as opposed to the cap
  /// cutting the loop short. Output is produced either way — see below.
  bool converged = false;
  /// Blocks the final function has that analysis::reachableBlocks() does not
  /// reach from the entry: a resolve-indirect candidate, or a discovery lifted
  /// from one, that turned out not to be reachable after all. Nothing
  /// downstream emits these (the structurizer only walks blocks Dominators
  /// itself reaches, by the same reasoning), so this is not a correctness
  /// problem by itself -- it is the double-check the jump-table candidate
  /// filtering is meant to make almost always zero, kept here so a caller or
  /// test can notice when it is not.
  std::size_t unreachableBlocks = 0;
};

struct DriverResult {
  std::unique_ptr<il::Function> function;
  DriverReport report;
};

/// Lifts `entry` and drives it to `options.target`, discovering blocks across
/// unresolved indirect branches as the rounds demand.
[[nodiscard]] Result<DriverResult> decompile(const spec::SpecEngine& engine,
                                             const ByteReader& reader, uint64_t entry,
                                             pass::Registry& registry,
                                             const DriverOptions& options);

}  // namespace xdec::decompile
