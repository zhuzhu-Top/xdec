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
  /// Only meaningful with Redirect. A model that has to regain control after
  /// the callee it redirected into returns -- a guest callback it chose to
  /// interpret rather than forward -- sets the address of a boundary to raise
  /// when that call's redirect frame pops. Zero keeps the plain behaviour of
  /// resuming the original caller directly.
  uint64_t continuationPc = 0;
  StatePatch state;
  std::vector<MemoryPatch> memory;
  /// The embedder already synchronized all current guest dirt and these effects
  /// with the real backing process. The patches still update guest truth and
  /// trace, but no byte remains pending for a later writeback.
  bool memoryAlreadySynchronized = false;
  /// The exact input bytes the embedder handed to the real call, captured
  /// before it ran, as write patches keyed by guest address. This is the
  /// authoritative "before" image of every input region: a recorder must use it
  /// rather than re-reading guest memory after the call, because a call that
  /// consumes or reuses an input buffer -- a Mach out-of-line send hands its
  /// pages to the kernel and the address is then reused by the next message --
  /// leaves that memory holding something other than what was sent. Empty for a
  /// handler that ran no real call, so a reader that finds no covering patch
  /// falls back to reading guest memory, which is correct when nothing moved.
  std::vector<MemoryPatch> boundaryInputs;
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
