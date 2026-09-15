#include "xdec/exec/session.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace xdec::exec {
namespace {

std::atomic<uint64_t> nextSessionId{1};

std::vector<std::pair<il::RegId, ConcreteValue>> captureRoots(
    const il::RegisterFile& registers, const il::Interpreter& interpreter) {
  std::vector<std::pair<il::RegId, ConcreteValue>> result;
  for (uint32_t index = 0; index < registers.size(); ++index) {
    const il::RegId reg{index};
    const il::RegisterInfo& info = registers[reg];
    if (info.regClass != il::RegClass::Zero && registers.rootOf(reg) == reg) {
      result.emplace_back(reg, interpreter.readRegister(reg));
    }
  }
  return result;
}

void importState(const MachineState& state, il::Interpreter& interpreter) {
  const il::RegisterFile& registers = state.registers();
  for (uint32_t index = 0; index < registers.size(); ++index) {
    const il::RegId reg{index};
    if (registers.rootOf(reg) == reg) {
      interpreter.writeRegister(reg, state.read(reg));
    }
  }
}

void exportState(const il::Interpreter& interpreter, MachineState& state) {
  const il::RegisterFile& registers = state.registers();
  for (uint32_t index = 0; index < registers.size(); ++index) {
    const il::RegId reg{index};
    if (registers.rootOf(reg) == reg) {
      state.write(reg, interpreter.readRegister(reg));
    }
  }
}

}  // namespace

std::string_view toString(SessionStop stop) noexcept {
  switch (stop) {
    case SessionStop::Returned: return "returned";
    case SessionStop::InstructionBudget: return "instruction-budget";
    case SessionStop::ExternalBoundary: return "external-boundary";
    case SessionStop::HandlerStop: return "handler-stop";
    case SessionStop::Fault: return "fault";
    case SessionStop::Unimplemented: return "unimplemented";
    case SessionStop::Unreachable: return "unreachable";
    case SessionStop::InvalidResume: return "invalid-resume";
    case SessionStop::InvalidConfiguration: return "invalid-configuration";
  }
  return "?";
}

ExecSession::ExecSession(const spec::SpecEngine& engine,
                         GuestAddressSpace& memory, MachineState initial,
                         ExecOptions options)
    : engine_(&engine),
      memory_(&memory),
      state_(std::move(initial)),
      options_(std::move(options)),
      sessionId_(nextSessionId.fetch_add(1, std::memory_order_relaxed)),
      configurationValid_(&state_.registers() == &engine.program().registers &&
                          options_.maxBlockInstructions != 0) {}

ExecResult ExecSession::run() {
  if (!configurationValid_) {
    return ExecResult{SessionStop::InvalidConfiguration, state_.pc(), executed_,
                      "machine state and execution engine use different register files, "
                      "or the block budget is zero"};
  }
  if (suspended_.has_value()) {
    return ExecResult{SessionStop::InvalidResume, state_.pc(), executed_,
                      "session is suspended; resume with its token"};
  }
  return runLoop();
}

ExecResult ExecSession::resume(ResumeToken token,
                               const ExternalResponse& response) {
  if (!suspended_.has_value() || token.session != sessionId_ ||
      token.generation != tokenGeneration_) {
    return ExecResult{SessionStop::InvalidResume, state_.pc(), executed_,
                      "resume token does not belong to this suspended session"};
  }
  const ExternalRequest request = *suspended_;
  if (auto applied = applyResponse(request, response); !applied) {
    observeEffect(request, nullptr, applied.error().format());
    ExecResult result{SessionStop::Fault, state_.pc(), executed_,
                      applied.error().format()};
    result.boundary = request;
    result.resume = token;
    return result;
  }
  observeEffect(request, &response);
  suspended_.reset();
  if (response.action == ExternalAction::Stop) {
    return suspend(request, SessionStop::HandlerStop);
  }
  return runLoop();
}

ExecResult ExecSession::suspend(ExternalRequest request, SessionStop reason) {
  suspended_ = request;
  ++tokenGeneration_;
  ExecResult result;
  result.stop = reason;
  result.pc = state_.pc();
  result.instructions = executed_;
  result.boundary = std::move(request);
  result.resume = ResumeToken{sessionId_, tokenGeneration_};
  return result;
}

Result<void> ExecSession::applyResponse(const ExternalRequest& request,
                                        const ExternalResponse& response) {
  memory_->setAccessObserver({});
  MachineState nextState = state_;
  XDEC_TRY_VOID(nextState.apply(response.state));
  XDEC_TRY_VOID(memory_->validate(response.memory));
  if (response.action == ExternalAction::Redirect) {
    if (request.kind == BoundaryKind::DirectCall ||
        request.kind == BoundaryKind::IndirectCall) {
      redirectedCallReturns_.push_back(request.returnPc);
    }
    nextState.setPc(response.nextPc);
  } else if (response.action == ExternalAction::ReturnFromCall) {
    // A redirected PLT/veneer can end in `br target`, so the real external
    // callee returns directly to the BL return address without executing a RET
    // in the redirected block. Consume that redirect frame here; otherwise the
    // next enclosing RET sees a stale stack entry and is mistaken for the
    // session's outermost return.
    if (!redirectedCallReturns_.empty() &&
        redirectedCallReturns_.back() == request.returnPc) {
      redirectedCallReturns_.pop_back();
    }
    nextState.setPc(request.returnPc);
  } else if (response.action == ExternalAction::HandledContinue) {
    nextState.setPc(request.returnPc);
  }
  XDEC_TRY_VOID(memory_->apply(response.memory));
  if (response.memoryAlreadySynchronized) {
    memory_->clearDirty();
  }
  state_ = std::move(nextState);
  return ok();
}

void ExecSession::observeEffect(const ExternalRequest& request,
                                const ExternalResponse* response,
                                std::string error) {
  if (observer_ == nullptr || !options_.trace.externalEffects) {
    return;
  }
  BoundaryEffectRecord record;
  record.boundary = BoundaryRecord{request.pc, request.returnPc, request.target,
                                   request.kind, request.name};
  record.nextPc = response == nullptr ? request.pc : state_.pc();
  record.error = std::move(error);
  if (response != nullptr) {
    record.action = response->action;
    record.state = response->state;
    record.memory = response->memory;
    record.memoryAlreadySynchronized = response->memoryAlreadySynchronized;
  }
  observer_->onBoundaryEffect(record);
}

ExecResult ExecSession::runLoop() {
  while (executed_ < options_.maxInstructions) {
    if (observer_ != nullptr &&
        options_.trace.maxInstructionRecords != 0 &&
        emitted_ >= options_.trace.maxInstructionRecords) {
      return ExecResult{SessionStop::InstructionBudget, state_.pc(), executed_,
                        "trace record budget exhausted"};
    }

    const uint64_t pc = state_.pc();
    const uint64_t executionRemaining = options_.maxInstructions - executed_;
    uint64_t traceRemaining = executionRemaining;
    if (observer_ != nullptr &&
        options_.trace.maxInstructionRecords != 0) {
      traceRemaining = options_.trace.maxInstructionRecords - emitted_;
    }
    const std::size_t blockBudget = static_cast<std::size_t>(
        std::min<uint64_t>({executionRemaining, traceRemaining,
                            options_.maxBlockInstructions}));

    std::shared_ptr<ExecBlock> block;
    if (const auto found = cache_.find(pc);
        found != cache_.end() && found->second->validFor(*memory_) &&
        found->second->instructions.size() <= blockBudget) {
      block = found->second;
    } else {
      auto compiled = compileExecBlock(*engine_, *memory_, pc, blockBudget);
      if (!compiled) {
        return ExecResult{SessionStop::Fault, pc, executed_,
                          compiled.error().format()};
      }
      block = std::move(*compiled);
      cache_[pc] = block;
    }

    il::Interpreter interpreter{*block->lifted.function, *memory_};
    importState(state_, interpreter);

    const il::RegisterFile& registers = state_.registers();
    std::optional<InstructionRecord> active;
    std::vector<std::pair<il::RegId, ConcreteValue>> before;
    std::vector<ConcreteValue> intrinsicArguments;
    std::string intrinsicName;

    const auto finalize = [&] {
      if (!active.has_value()) {
        return;
      }
      if (options_.trace.registerDeltas) {
        for (const auto& [reg, oldValue] : before) {
          const ConcreteValue newValue = interpreter.readRegister(reg);
          if (oldValue != newValue) {
            active->registers.push_back(
                RegisterDelta{reg, oldValue, newValue});
          }
        }
      }
      ++executed_;
      active->sequence = executed_;
      if (observer_ != nullptr) {
        observer_->onInstruction(*active);
        ++emitted_;
      }
      active.reset();
      before.clear();
    };

    interpreter.setInterruptHook([&](const il::Op& op) {
      return active.has_value() && active->pc != op.va &&
             !block->validFor(*memory_);
    });
    interpreter.setOpHook([&](const il::Op& op) {
      if (!active.has_value() || active->pc != op.va) {
        finalize();
        active.emplace();
        active->pc = op.va;
        if (const ExecInstruction* instruction = block->instructionAt(op.va);
            instruction != nullptr) {
          active->word = instruction->word;
          active->length = instruction->length;
          if (options_.trace.disassembly) {
            active->disassembly = instruction->disassembly;
          }
        }
        before = captureRoots(registers, interpreter);
      }
      memory_->setInstructionContext(op.va);
    });
    memory_->setAccessObserver([&](const MemoryAccess& access) {
      if (active.has_value() && options_.trace.memoryAccesses) {
        MemoryAccess captured = access;
        if (!options_.trace.memoryValues) {
          captured.value = {};
        }
        if (options_.trace.memoryContextBytes != 0) {
          const uint64_t half = options_.trace.memoryContextBytes / 2;
          captured.contextAddress =
              access.address < half ? 0 : access.address - half;
          captured.context.resize(options_.trace.memoryContextBytes);
          if (auto read = memory_->readBytes(captured.contextAddress,
                                             captured.context);
              !read) {
            captured.context.clear();
            captured.contextAddress = 0;
          }
        }
        active->memory.push_back(std::move(captured));
      }
    });
    interpreter.setIntrinsicHook(
        [&](std::string_view name, il::Type,
            std::span<const ConcreteValue> arguments, ConcreteValue&) {
          intrinsicName = std::string{name};
          intrinsicArguments.assign(arguments.begin(), arguments.end());
          return false;
        });

    const il::ExecOutcome outcome =
        interpreter.runBlock(block->lifted.block);
    finalize();
    memory_->setAccessObserver({});
    exportState(interpreter, state_);

    if (outcome.stop == il::ExecStop::Branch) {
      if (options_.externalBranch &&
          options_.externalBranch(outcome.va, outcome.target)) {
        const auto lr = state_.read("x30");
        if (!lr) {
          return ExecResult{SessionStop::Fault, outcome.va, executed_,
                            lr.error().format()};
        }
        ExternalRequest request;
        request.kind = BoundaryKind::DirectCall;
        request.pc = outcome.va;
        request.returnPc = lr->lo;
        request.target = outcome.target;
        request.dirty = memory_->dirtyRanges();
        if (observer_ != nullptr) {
          observer_->onBoundary(BoundaryRecord{request.pc, request.returnPc,
                                               request.target, request.kind,
                                               "tail-call"});
        }
        state_.setPc(request.pc);
        if (handler_ == nullptr) {
          return suspend(std::move(request), SessionStop::ExternalBoundary);
        }
        auto response = handler_->handle(request, state_, *memory_);
        if (!response) {
          observeEffect(request, nullptr, response.error().format());
          ExecResult result = suspend(request, SessionStop::HandlerStop);
          result.detail = response.error().format();
          return result;
        }
        if (auto applied = applyResponse(request, *response); !applied) {
          observeEffect(request, nullptr, applied.error().format());
          ExecResult result = suspend(request, SessionStop::Fault);
          result.detail = applied.error().format();
          return result;
        }
        observeEffect(request, &*response);
        if (response->action == ExternalAction::Stop) {
          return suspend(std::move(request), SessionStop::HandlerStop);
        }
        continue;
      }
      state_.setPc(outcome.target);
      continue;
    }
    if (outcome.stop == il::ExecStop::CondBranch) {
      state_.setPc(outcome.condition ? outcome.target : outcome.fallthrough);
      continue;
    }
    if (outcome.stop == il::ExecStop::IndirectBranch) {
      if (options_.externalBranch &&
          options_.externalBranch(outcome.va, outcome.target)) {
        const auto lr = state_.read("x30");
        if (!lr) {
          return ExecResult{SessionStop::Fault, outcome.va, executed_,
                            lr.error().format()};
        }
        ExternalRequest request;
        request.kind = BoundaryKind::IndirectCall;
        request.pc = outcome.va;
        request.returnPc = lr->lo;
        request.target = outcome.target;
        request.dirty = memory_->dirtyRanges();
        if (observer_ != nullptr) {
          observer_->onBoundary(BoundaryRecord{request.pc, request.returnPc,
                                               request.target, request.kind,
                                               "tail-call"});
        }
        state_.setPc(request.pc);
        if (handler_ == nullptr) {
          return suspend(std::move(request), SessionStop::ExternalBoundary);
        }
        auto response = handler_->handle(request, state_, *memory_);
        if (!response) {
          observeEffect(request, nullptr, response.error().format());
          ExecResult result = suspend(request, SessionStop::HandlerStop);
          result.detail = response.error().format();
          return result;
        }
        if (auto applied = applyResponse(request, *response); !applied) {
          observeEffect(request, nullptr, applied.error().format());
          ExecResult result = suspend(request, SessionStop::Fault);
          result.detail = applied.error().format();
          return result;
        }
        observeEffect(request, &*response);
        if (response->action == ExternalAction::Stop) {
          return suspend(std::move(request), SessionStop::HandlerStop);
        }
        continue;
      }
      state_.setPc(outcome.target);
      continue;
    }
    if (outcome.stop == il::ExecStop::Interrupted) {
      state_.setPc(outcome.va);
      continue;
    }
    if (outcome.stop == il::ExecStop::Return) {
      const auto lr = state_.read("x30");
      if (!redirectedCallReturns_.empty() && lr &&
          lr->lo == redirectedCallReturns_.back()) {
        state_.setPc(redirectedCallReturns_.back());
        redirectedCallReturns_.pop_back();
        continue;
      }
      state_.setPc(outcome.va);
      return ExecResult{SessionStop::Returned, state_.pc(), executed_};
    }
    if (outcome.stop == il::ExecStop::Unreachable) {
      return ExecResult{SessionStop::Unreachable, state_.pc(), executed_,
                        "execution reached an unreachable instruction"};
    }
    if (outcome.stop == il::ExecStop::Unimplemented) {
      return ExecResult{SessionStop::Unimplemented, outcome.va, executed_,
                        outcome.detail};
    }
    if (outcome.stop == il::ExecStop::Error) {
      return ExecResult{SessionStop::Fault, outcome.va, executed_,
                        outcome.detail};
    }

    const ExecInstruction* instruction = block->instructionAt(outcome.va);
    ExternalRequest request;
    request.pc = outcome.va;
    request.returnPc =
        outcome.va + (instruction == nullptr ? engine_->program().insnWidth / 8
                                             : instruction->length);
    request.dirty = memory_->dirtyRanges();

    if (outcome.stop == il::ExecStop::Call) {
      request.kind =
          instruction != nullptr && instruction->flow.callTargetKnown
              ? BoundaryKind::DirectCall
              : BoundaryKind::IndirectCall;
      request.target = outcome.target;
    } else {
      request.kind = intrinsicName == "aarch64.svc"
                         ? BoundaryKind::Syscall
                         : BoundaryKind::Intrinsic;
      request.name =
          intrinsicName.empty() ? outcome.detail : intrinsicName;
      request.arguments = std::move(intrinsicArguments);
      if (request.kind == BoundaryKind::Syscall &&
          request.arguments.size() > 1) {
        request.target = request.arguments[1].lo;
      }
    }

    if (observer_ != nullptr) {
      observer_->onBoundary(BoundaryRecord{request.pc, request.returnPc,
                                           request.target, request.kind,
                                           request.name});
    }
    state_.setPc(request.pc);
    if (handler_ == nullptr) {
      return suspend(std::move(request), SessionStop::ExternalBoundary);
    }

    auto response = handler_->handle(request, state_, *memory_);
    if (!response) {
      observeEffect(request, nullptr, response.error().format());
      ExecResult result = suspend(request, SessionStop::HandlerStop);
      result.detail = response.error().format();
      return result;
    }
    if (auto applied = applyResponse(request, *response); !applied) {
      observeEffect(request, nullptr, applied.error().format());
      ExecResult result = suspend(request, SessionStop::Fault);
      result.detail = applied.error().format();
      return result;
    }
    observeEffect(request, &*response);
    if (response->action == ExternalAction::Stop) {
      return suspend(std::move(request), SessionStop::HandlerStop);
    }
  }
  return ExecResult{SessionStop::InstructionBudget, state_.pc(), executed_,
                    "instruction budget exhausted"};
}

}  // namespace xdec::exec
