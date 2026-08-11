// Recognises the two-IL-op split `specs/arm64/loadstore.xspec` uses for
// AArch64's exclusive load/store (`ldaxr`/`stlxr`): the reservation an
// exclusive load sets, and the status an exclusive store's write keeps or
// loses, are real effects the IL does not model as data flow, so the spec
// wraps each as an intrinsic around an ordinary Load or Store rather than
// inventing semantics for them. That split is exactly the shape a reader
// asked to reassemble by hand -- two statements next to each other, one of
// them meaningless in C on its own -- so this file finds the pairs back,
// the same way `analysis::findFoldableMemoryLoads` finds a load with no
// business being its own statement. What to do with a found pair (print the
// one ACLE call it really is, fold the earlier op into `deadOps`) is
// c_context.cpp's decision; this only locates the shape.
#pragma once

#include <cstdint>
#include <unordered_map>

#include "xdec/il/function.h"

namespace xdec::analysis {

/// A `Load` immediately preceded, in its own block, by
/// `intrinsic "aarch64.reserve"(address)` reading the load's own address.
struct ExclusiveLoad {
  il::OpId reserveOp;  ///< the intrinsic to fold away (joins deadOps)
};

/// Every Load eligible for the ldaxr fold, keyed by the load's own
/// `OpId::index()`. There is no farther-apart shape to look for:
/// `loadstore.xspec`'s `ldaxr` rule always emits the pair adjacent, in this
/// order, and no pass between lifting and structuring reorders ops within a
/// block.
[[nodiscard]] std::unordered_map<uint32_t, ExclusiveLoad> findExclusiveLoads(
    const il::Function& function);

/// A `Store` immediately followed, in its own block, by
/// `%r = intrinsic "aarch64.store_exclusive_status"()`.
struct ExclusiveStore {
  il::OpId statusOp;  ///< the intrinsic that reads the store's address/value
};

/// Every Store eligible for the stlxr fold, keyed by the store's own
/// `OpId::index()`. Mirrors `findExclusiveLoads`, split the other way round
/// because `stlxr`'s status is read AFTER the write it reports on.
[[nodiscard]] std::unordered_map<uint32_t, ExclusiveStore> findExclusiveStores(
    const il::Function& function);

}  // namespace xdec::analysis
