// Lifting a basic block's worth of bytes into an IL function.
//
// `xdec lift` demonstrates the probe-then-elaborate flow on a window of
// instructions where every branch target must stay inside the window. That is
// the right shape for a CLI demo and the wrong shape for a driver: a basic
// block's terminator almost always points somewhere the caller did not lift.
// liftBasicBlock is the reusable form, and it is also what the interpreter and
// the Unicorn differential consume.
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/spec/engine.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::spec {

struct LiftedBlock {
  std::unique_ptr<il::Function> function;
  /// The block starting at the range's base address.
  il::BlockId block;
};

/// Lifts the bytes as one basic block.
///
/// Branch targets outside the range become boundary stubs: blocks that carry
/// their address and nothing else, so a terminator's edges resolve to real
/// BlockIds whose addresses a caller can read back. A range whose last
/// instruction falls through ends in a synthesized branch to a stub at the
/// first unlisted address, so every lifted block has a terminator and the
/// interpreter always has a stop to report.
///
/// The result is intentionally not verified: the stubs have no terminators,
/// which the verifier would rightly reject for a whole function.
[[nodiscard]] Result<LiftedBlock> liftBasicBlock(const SpecEngine& engine,
                                                 std::span<const std::byte> bytes,
                                                 uint64_t base);

/// Memory the lifter reads instruction bytes from. The alias lives in
/// support/reader.h so layers below the spec (passes, analyses) can take a
/// byte source without depending on the lifter.
using ByteReader = xdec::ByteReader;

struct LiftedFunction {
  std::unique_ptr<il::Function> function;
  il::BlockId entry;
  /// Blocks whose scan stopped on an indirect branch, an undecodable word, or
  /// an unmapped address. These are the edges a later resolution pass (the
  /// deflattening one, say) has to close; listing them beats rediscovering.
  std::vector<uint64_t> unresolved;
  /// Direct branch targets outside the discoverable function — tail calls into
  /// the unmapped, mostly. Each got an empty stub block so the branch resolves
  /// to a real BlockId; the addresses are listed here so nothing mistakes a
  /// stub for a block with content.
  std::vector<uint64_t> external;
};

/// Recursive-descent function discovery: starts at `entry`, follows direct
/// branches to fixpoint, and lifts every reachable block. Indirect branches
/// end their blocks unresolved, by design — see FlowKind::IndirectBranch.
///
/// Phase one probes for block leaders and extents only; a target landing
/// strictly inside an already-scanned block rescans it with the finer
/// boundary. Phase two elaborates each extent. Blocks cut short by a
/// mid-instruction leader get a synthesized fall-through branch, attributed
/// to the last real instruction like every other op.
///
/// The function comes back at Maturity::Lifted: blocks carry addresses and
/// terminators, but no promise of CFG completeness until the unresolved edges
/// are resolved.
[[nodiscard]] Result<LiftedFunction> liftFunction(const SpecEngine& engine,
                                                  const ByteReader& reader,
                                                  uint64_t entry);

/// Incremental discovery into an existing function: scans and elaborates
/// basic blocks starting at `entries`, splicing them into `function` next to
/// the blocks already there. This is how indirect-branch resolution closes
/// the discovery gap: a resolved target without a block becomes one.
///
/// Existing blocks are treated as boundaries (a scan stops at them) and as
/// split candidates (a new leader inside one splits it, same contract as
/// liftFunction). Entries that are already block starts, or do not read as
/// mapped memory, are skipped silently — resolution offers candidates, the
/// lifter decides what is real.
struct LiftBlocksResult {
  /// Entries (and extents) that became blocks, in discovery order.
  std::vector<uint64_t> lifted;
  /// Block starts whose scan stopped on an indirect branch or an unmapped
  /// word, same meaning as LiftedFunction::unresolved.
  std::vector<uint64_t> unresolved;
};
[[nodiscard]] Result<LiftBlocksResult> liftBlocksInto(il::Function& function,
                                                      const SpecEngine& engine,
                                                      const ByteReader& reader,
                                                      std::span<const uint64_t> entries);

}  // namespace xdec::spec
