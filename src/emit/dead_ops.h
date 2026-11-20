// Pre-emission dead-op identification over a StructuredFunction.
//
// Identifies vestigial IL ops (dead jump-table address loads, state-discriminant
// stores folded into `switch (var = ...)`, duplicate routing stores, and
// foldable errno-accessor calls) once up front so both variable declaration and
// AST lowering agree on what does not print.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_set>

#include "c_context.h"

namespace xdec::emit {

[[nodiscard]] il::OpId deadJumpTableLoad(const il::Function& function, il::BlockId block,
                                         bool tableMode, il::ExprId cond);

[[nodiscard]] il::OpId deadStateDiscriminantStore(const il::Function& function,
                                                  const analysis::StackFrame& frame,
                                                  const analysis::VariableTable& variables,
                                                  bool enumerated, il::BlockId block,
                                                  il::ExprId cond);

[[nodiscard]] il::OpId deadRoutingStateStore(const il::Function& function,
                                             const analysis::StackFrame& frame,
                                             const analysis::VariableTable& variables,
                                             il::BlockId block, il::ExprId ifCond,
                                             bool ifArmsLeaveDirectly);

/// Value defined by an import-accessor call when `store` matches `*(T*)t = -x`.
[[nodiscard]] std::optional<il::ValueId> importAccessorStoreValue(const il::Function& function,
                                                                  const il::Op& store);

[[nodiscard]] std::unordered_set<uint32_t> collectDeadOps(const il::Function& function,
                                                          const analysis::StackFrame& frame,
                                                          const analysis::VariableTable& variables,
                                                          const Stmt& root,
                                                          const COptions& options);

}  // namespace xdec::emit
