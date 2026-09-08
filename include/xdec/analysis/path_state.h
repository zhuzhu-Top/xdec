// PathState: one in-flight symbolic walk's local knowledge.
//
// analysis::ImageEval evaluates an expression once, whole-function, treating
// every phi as the union of all its incoming edges and every load as a read
// of the image (never of something this function itself stored). Both are
// the right defaults for a single-shot "what could this be" question, and
// both are exactly what a *path* -- one concrete walk from the entry, having
// already taken specific edges and executed specific stores -- knows more
// than: at a phi, a path arrived along exactly one edge, so the value there
// is that edge's operand, not a union; and a load may read bytes a `Store`
// earlier on the very same path put there, which is not in the image at all.
//
// PathState is the state one such walk carries: which SSA value each
// executed op resolved to (`values_`), and which addresses this path's own
// stores have written (`localStore_`). It is cheap to copy on purpose --
// PathExplorer clones one of these at every conditional fork, and a bounded
// number of forked paths over a bounded function is the whole safety
// argument, so cloning a couple of hash maps had better be inexpensive.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>

#include "xdec/analysis/sym_value.h"
#include "xdec/il/function.h"
#include "xdec/support/reader.h"

namespace xdec::analysis {

class EntryRegFacts;

/// One path's own memory effects: addresses it has written since it forked
/// off the entry, each remembered at the width it was written with. A read
/// at a different width than the write on record is not answered from here
/// -- this path knows less than a byte-precise memory model would, and
/// admitting that is better than guessing.
struct PathStoreRecord {
  SymValueSet value;
  unsigned width = 0;
};

/// The state one path carries from the entry to wherever it currently sits.
/// Copyable by design (see the header); every member is a value type.
class PathState {
 public:
  PathState(const il::Function& function, ByteReader reader, const EntryRegFacts* entryRegs)
      : function_(&function), reader_(std::move(reader)), entryRegs_(entryRegs) {}

  /// The values `id` can take, given what this path has resolved of its own
  /// SSA values and local stores so far. Mirrors ImageEval::eval's op
  /// handling (arithmetic, casts, select, EntryReg) exactly; the two points
  /// where this differs -- Value and Load -- are documented on evalValue and
  /// loadFrom below.
  [[nodiscard]] SymValueSet eval(il::ExprId id);

  /// Binds `id` (a Phi's result) to the value flowing in along the one edge
  /// this path actually took, called once per Phi as PathInterpreter walks a
  /// block in program order. Unlike ImageEval's union-of-all-arms reading,
  /// this is the path-sensitive answer: a canary check's two arms may leave
  /// a merged value very different sets, and only one of them is what this
  /// walk actually saw.
  void bindPhi(il::ValueId id, il::ExprId incoming);
  /// As above, when the incoming edge could not be identified (predecessor
  /// bookkeeping stale mid-pass -- see path_explorer.cpp's construction-time
  /// snapshot for why this is rare, not absent). Degrades to top, never a
  /// guess.
  void bindPhiUnknown(il::ValueId id);

  /// Binds `id` (a Load's result) to what this path can prove about the
  /// bytes at `address`: this path's own prior store there first, then the
  /// image's own bytes -- see loadFrom.
  void bindLoad(il::ValueId id, il::Type type, il::ExprId address);

  /// Records that this path just wrote `value` to `address` at `type`'s
  /// width, for a later bindLoad on the same path to read back. An address
  /// this path cannot pin to a small concrete set invalidates every record
  /// this path is holding (the store's true target might have been any of
  /// them) rather than keep now-unreliable memory around.
  void recordStore(il::ExprId address, il::Type type, il::ExprId value);

  /// How many times this path has visited `block`, for PathExplorer's loop
  /// bound (see PathExploreOptions::maxBlockRevisits).
  [[nodiscard]] unsigned blockVisits(il::BlockId block) const;
  void markVisited(il::BlockId block);

  /// Total blocks this path has stepped through since the entry, for
  /// PathExploreOptions::maxStepsPerPath.
  unsigned steps = 0;

 private:
  [[nodiscard]] SymValueSet evalValue(il::ValueId id);
  [[nodiscard]] SymValueSet evalUnary(const il::Expr& expr);
  [[nodiscard]] SymValueSet evalBinary(const il::Expr& expr);
  [[nodiscard]] SymValueSet evalSelect(const il::Expr& expr);
  [[nodiscard]] SymValueSet evalCast(const il::Expr& expr);
  [[nodiscard]] SymValueSet evalEntryReg(const il::Expr& expr);
  /// Reads through this path's own store records first, then the image --
  /// the same priority ImageEval::loadFrom documents, with this path's own
  /// writes now outranking it (a value this path itself just computed and
  /// spilled is more specific than anything the image on disk could say).
  [[nodiscard]] SymValueSet loadFrom(const SymValueSet& addresses, il::Type type);

  const il::Function* function_;
  ByteReader reader_;
  const EntryRegFacts* entryRegs_;

  /// ValueId index -> resolved value, valid for this path only.
  std::unordered_map<uint32_t, SymValueSet> values_;
  /// ExprId index -> memoised eval, valid for this path only (an id may
  /// legitimately answer differently on two different paths, since it can
  /// read a Value this path bound to something another path did not).
  std::unordered_map<uint32_t, SymValueSet> exprMemo_;
  std::unordered_map<uint64_t, PathStoreRecord> localStore_;
  std::unordered_map<uint32_t, unsigned> blockVisits_;
};

}  // namespace xdec::analysis
