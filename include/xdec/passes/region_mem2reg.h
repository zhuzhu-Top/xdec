// RegionMem2Reg & RegionDSE: generic SSA promotion and dead-store elimination
// for non-escaping constant-offset slots inside declared `PrivateRegFile` and
// `PrivateStack` regions (`devirt::MemoryRegionMap`).
#pragma once

#include <memory>

#include "xdec/pass/pass.h"

namespace xdec::passes {

[[nodiscard]] std::unique_ptr<pass::Pass> makeRegionMem2RegPass();
[[nodiscard]] std::unique_ptr<pass::Pass> makeRegionDsePass();

}  // namespace xdec::passes
