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
#include "xdec/spec/engine.h"

namespace xdec::exec {

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
  uint64_t maxInstructions = 100000;
  std::size_t maxBlockInstructions = 256;
  TracePolicy trace;
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
  std::unordered_map<uint64_t, std::shared_ptr<ExecBlock>> cache_;
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
