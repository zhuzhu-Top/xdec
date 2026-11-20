// Out-of-SSA phi edge lowering: computes parallel edge copies, dispatcher
// LiveRegisterFrame suppression, and `__prev` conflict snapshots.
#pragma once

#include <string>
#include <vector>

#include "c_context.h"
#include "xdec/analysis/live_register_frame.h"

namespace xdec::emit {

struct EdgeCopy {
  il::ValueId destination;
  std::string name;
  std::string type;
  il::ExprId source;
};

struct ActiveFrame {
  il::BlockId merge;
  analysis::LiveRegisterFrame frame;
  std::vector<bool> unanimous;
};

struct EdgePlan {
  std::vector<EdgeCopy> copies;
  std::vector<const EdgeCopy*> snapshots;  // pointers into `copies`
  bool scoped = false;

  [[nodiscard]] bool empty() const noexcept { return copies.empty(); }
  [[nodiscard]] bool needsSnapshot(const EdgeCopy& copy) const noexcept;
};

[[nodiscard]] std::vector<EdgeCopy> collectEdgeCopies(const CContext& ctx, il::BlockId from,
                                                      il::BlockId to);

[[nodiscard]] EdgePlan buildEdgePlan(const CContext& ctx, il::BlockId from, il::BlockId to,
                                     const ActiveFrame* activeFrame);

[[nodiscard]] il::BlockId firstBlockOf(const Stmt* stmt);
[[nodiscard]] const il::Op* terminatorOf(const il::Function& function, il::BlockId block);

}  // namespace xdec::emit
