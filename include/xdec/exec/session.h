// Cross-block deterministic execution over xspec-produced semantics.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "xdec/exec/exec_ir.h"
#include "xdec/exec/external.h"
#include "xdec/exec/memory.h"
#include "xdec/exec/state.h"
#include "xdec/exec/trace.h"
#include "xdec/il/interp.h"
#include "xdec/spec/engine.h"
#include "xdec/support/perf_stats.h"

namespace xdec::exec {

/// Per-stage timing for one session's runLoop. All optional: a caller that
/// never calls setPerfStats leaves every ScopedTimer a null check away from
/// a no-op, so profiling is opt-in with no cost for embedders who skip it.
struct ExecSessionPerfStats {
  /// The whole runLoop, so the stage counters below can be checked against a
  /// measured total instead of against wall time the caller also spends
  /// elsewhere.
  PerfCounter runLoop;
  /// Block-cache probe plus the code-page generation check.
  PerfCounter blockLookup;
  /// decode + lift + disassemble on a block-cache miss.
  PerfCounter blockCompile;
  /// Rebinding the interpreter to the next block's lifted function.
  PerfCounter blockBind;
  /// Reinstalling the per-block trace hooks on the interpreter and memory.
  PerfCounter hookInstall;
  /// MachineState -> interpreter register copy after a boundary resumes.
  PerfCounter stateImport;
  /// interpreter -> MachineState register copy, once per executed block.
  PerfCounter stateExport;
  /// il::Interpreter::runBlock, one call per executed block.
  PerfCounter ilExecute;
  /// Register delta capture/diff across the executed instructions.
  PerfCounter registerTrace;
  /// Time spent inside the attached IExecObserver's callbacks.
  PerfCounter observer;
  /// Collecting dirty guest ranges to attach to an external boundary request.
  PerfCounter dirtyScan;
};

enum class SessionStop : uint8_t {
  Returned,
  InstructionBudget,
  ExternalBoundary,
  HandlerStop,
  Fault,
  Unimplemented,
  Unreachable,
  InvalidResume,
  InvalidConfiguration,
};

[[nodiscard]] std::string_view toString(SessionStop stop) noexcept;

struct ExecOptions {
  // Zero means no instruction budget: run until return, fault, or handler stop.
  uint64_t maxInstructions = 0;
  std::size_t maxBlockInstructions = 256;
  TracePolicy trace;
  // Whether ExternalRequest::dirty is filled in. The dirty set spans the whole
  // session, so materializing it costs O(bytes written so far) on every
  // boundary; an embedder that tells the far side exactly which ranges to
  // synchronize never reads it and should not pay for it.
  bool externalDirtyRanges = true;
  // A client-supplied code-domain boundary. Taken branches for which this
  // returns true are treated as tail calls into an external environment.
  std::function<bool(uint64_t from, uint64_t target)> externalBranch;
};

struct ExecResult {
  ExecResult() = default;
  ExecResult(SessionStop reason, uint64_t address, uint64_t count,
             std::string message = {})
      : stop(reason),
        pc(address),
        instructions(count),
        detail(std::move(message)) {}

  SessionStop stop = SessionStop::Fault;
  uint64_t pc = 0;
  uint64_t instructions = 0;
  std::string detail;
  std::optional<ExternalRequest> boundary;
  ResumeToken resume;
};

/// Owns control-flow progress while the caller owns memory and environment
/// policy. A suspended session can only be resumed with its issued token.
class ExecSession {
 public:
  ExecSession(const spec::SpecEngine& engine, GuestAddressSpace& memory,
              MachineState initial, ExecOptions options = {});

  void setObserver(IExecObserver* observer) noexcept { observer_ = observer; }
  void setExternalHandler(IExternalHandler* handler) noexcept { handler_ = handler; }
  void setPerfStats(ExecSessionPerfStats* stats) noexcept { perfStats_ = stats; }

  [[nodiscard]] MachineState& state() noexcept { return state_; }
  [[nodiscard]] const MachineState& state() const noexcept { return state_; }
  [[nodiscard]] GuestAddressSpace& memory() noexcept { return *memory_; }

  [[nodiscard]] ExecResult run();
  [[nodiscard]] ExecResult resume(ResumeToken token,
                                  const ExternalResponse& response);

  [[nodiscard]] std::size_t cachedBlockCount() const noexcept {
    return cache_.size();
  }

 private:
  [[nodiscard]] ExecResult runLoop();
  [[nodiscard]] ExecResult suspend(ExternalRequest request,
                                   SessionStop reason);
  [[nodiscard]] Result<void> applyResponse(const ExternalRequest& request,
                                           const ExternalResponse& response);
  void observeEffect(const ExternalRequest& request,
                     const ExternalResponse* response,
                     std::string error = {});

  const spec::SpecEngine* engine_;
  GuestAddressSpace* memory_;
  MachineState state_;
  ExecOptions options_;
  IExecObserver* observer_ = nullptr;
  IExternalHandler* handler_ = nullptr;
  ExecSessionPerfStats* perfStats_ = nullptr;
  std::unordered_map<uint64_t, std::shared_ptr<ExecBlock>> cache_;
  // The register file's full registers, resolved once. State transfer only
  // ever touches roots, and re-deriving them per block meant walking every
  // sub-register view (w0, s0, d0, ...) and its parent chain on a path that
  // runs once per executed block.
  std::vector<il::RegId> rootRegisters_;
  // Persists across blocks (and across suspend/resume) instead of being
  // rebuilt per block: construction alone re-zeros the whole register file,
  // which is exactly the O(register count) cost dirty tracking removed
  // elsewhere. rebind() retargets it at each new block's Function cheaply.
  std::unique_ptr<il::Interpreter> interpreter_;
  // Keeps interpreter_'s current Function alive independent of cache_: a
  // shrinking trace budget can recompile and overwrite cache_[pc] for the
  // same address interpreter_ is still bound to, and without this the old
  // ExecBlock (and its Function) would be destroyed out from under it.
  std::shared_ptr<ExecBlock> boundBlock_;
  // True whenever state_ was changed by something the interpreter's own
  // registers_ don't know about (session construction, or an external
  // handler's response applied in applyResponse()), so the next block must
  // import state_ before running instead of trusting stale registers.
  bool stateNeedsImport_ = true;
  // Redirect responses may enter an in-module callee. Ret instructions are
  // otherwise the public-session terminator, so retain their expected LR
  // values to resume the caller rather than ending the whole session.
  std::vector<uint64_t> redirectedCallReturns_;
  uint64_t executed_ = 0;
  uint64_t emitted_ = 0;
  uint64_t sessionId_ = 0;
  uint64_t tokenGeneration_ = 0;
  std::optional<ExternalRequest> suspended_;
  bool configurationValid_ = true;
};

}  // namespace xdec::exec
