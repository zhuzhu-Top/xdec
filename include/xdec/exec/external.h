// Explicit boundaries between machine execution and environment models.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xdec/exec/memory.h"
#include "xdec/exec/state.h"
#include "xdec/exec/trace.h"

namespace xdec::exec {

struct ExternalRequest {
  BoundaryKind kind = BoundaryKind::Intrinsic;
  uint64_t pc = 0;
  uint64_t returnPc = 0;
  uint64_t target = 0;
  std::string name;
  std::vector<ConcreteValue> arguments;
  std::vector<MemoryRange> dirty;
};

struct ExternalResponse {
  ExternalAction action = ExternalAction::HandledContinue;
  uint64_t nextPc = 0;
  StatePatch state;
  std::vector<MemoryPatch> memory;
  /// The embedder already synchronized all current guest dirt and these effects
  /// with the real backing process. The patches still update guest truth and
  /// trace, but no byte remains pending for a later writeback.
  bool memoryAlreadySynchronized = false;
};

class IExternalHandler {
 public:
  virtual ~IExternalHandler() = default;

  /// The state and memory are read-only snapshots from the session. All
  /// effects must be returned explicitly in ExternalResponse.
  [[nodiscard]] virtual Result<ExternalResponse> handle(
      const ExternalRequest& request, const MachineState& state,
      const GuestAddressSpace& memory) = 0;
};

struct ResumeToken {
  uint64_t session = 0;
  uint64_t generation = 0;

  friend bool operator==(ResumeToken lhs, ResumeToken rhs) noexcept = default;
  [[nodiscard]] bool valid() const noexcept { return session != 0; }
};

}  // namespace xdec::exec
