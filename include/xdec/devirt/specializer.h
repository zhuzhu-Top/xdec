// Context-sensitive partial evaluation engine (`Specializer`).
//
// Unrolls / specializes an `il::Function` (at Cfg or Lifted maturity) keyed by
// `(src_block, StateKey)`:
//   * Tracks concrete constant register & expression values along each path;
//   * Evaluates immutable memory loads via `LayeredByteReader`;
//   * Folds concrete `CondBranch` and `IndirectBranch` targets into direct edges;
//   * Reconnects back-edges whenever `(targetBlock, nextStateKey)` was already
//     visited, turning OLLVM state machines and rolling-key bytecode loops into
//     clean residual `il::Function`s ready for `decompileFromIL()`.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdec/devirt/memory.h"
#include "xdec/il/function.h"
#include "xdec/support/result.h"

namespace xdec::devirt {

struct SpecializerState {
  /// Concrete values of root registers (`RegId.index() -> uint64_t`).
  std::unordered_map<uint32_t, uint64_t> regValues;

  [[nodiscard]] std::optional<uint64_t> get(il::RegId reg) const noexcept {
    if (const auto it = regValues.find(reg.index()); it != regValues.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  void set(il::RegId reg, uint64_t value) { regValues[reg.index()] = value; }
  void clear(il::RegId reg) { regValues.erase(reg.index()); }
};

using StateKeyFn =
    std::function<std::string(uint64_t pc, const SpecializerState& state)>;

struct SpecializerOptions {
  /// Initial concrete register bindings at function entry.
  SpecializerState initialState;
  /// Root registers whose concrete values automatically form the `StateKey`
  /// when `stateKeyFn` is unset.
  std::vector<il::RegId> stateRegs;
  /// Optional custom key function `(pc, state) -> key`.
  StateKeyFn stateKeyFn;
  /// Optional layered memory for folding immutable constant reads.
  const LayeredByteReader* memory = nullptr;
  /// Maximum number of `(block, stateKey)` specialized nodes to emit.
  std::size_t maxNodes = 4096;
};

class Specializer {
 public:
  [[nodiscard]] static Result<std::unique_ptr<il::Function>> run(
      const il::Function& input, const SpecializerOptions& options = {});
};

}  // namespace xdec::devirt
