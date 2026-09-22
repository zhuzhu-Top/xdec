#include "xdec/exec/session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <span>
#include <utility>

namespace xdec::exec {
namespace {

std::atomic<uint64_t> nextSessionId{1};

void importState(const MachineState& state, il::Interpreter& interpreter,
                 std::span<const il::RegId> roots) {
  for (const il::RegId reg : roots) {
    interpreter.writeRegister(reg, state.read(reg));
  }
}

void exportState(const il::Interpreter& interpreter, MachineState& state,
                 std::span<const il::RegId> roots) {
  for (const il::RegId reg : roots) {
    state.write(reg, interpreter.readRegister(reg));
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
                          options_.maxBlockInstructions != 0) {
  const il::RegisterFile& registers = state_.registers();
  rootRegisters_.reserve(registers.size());
  for (uint32_t index = 0; index < registers.size(); ++index) {
    const il::RegId reg{index};
    if (registers.rootOf(reg) == reg) {
      rootRegisters_.push_back(reg);
    }
  }
}

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
  stateNeedsImport_ = true;
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
  const ScopedTimer loopTimer{perfStats_ == nullptr ? nullptr
                                                    : &perfStats_->runLoop};
  const auto collectDirty = [this] {
    if (!options_.externalDirtyRanges) return std::vector<MemoryRange>{};
    const ScopedTimer dirtyTimer{perfStats_ == nullptr ? nullptr
                                                       : &perfStats_->dirtyScan};
    return memory_->dirtyRanges();
  };
  const uint64_t instructionLimit = options_.maxInstructions == 0
                                        ? std::numeric_limits<uint64_t>::max()
                                        : options_.maxInstructions;
  // Outlives the block loop so its buffers survive block boundaries too.
  // finalize() leaves activeValid false, so a new block always starts clean.
  InstructionRecord active;
  bool activeValid = false;
  // Pins whatever block `active.semantics` points into. A record is finalized
  // at the start of the *next* instruction, which may be in the next block, and
  // by then cache_[pc] can already have been overwritten by a recompile -- the
  // same hazard boundBlock_ exists for. Compared before assigning, so this
  // costs one pointer test per instruction and a refcount only per block.
  std::shared_ptr<ExecBlock> activeBlock;
  // Where active.registers switches from source operands to destinations. The
  // sources are complete as soon as they are captured; the destinations need a
  // second read once the instruction has run.
  std::size_t writeDeltaBase = 0;
  while (executed_ < instructionLimit) {
    if (observer_ != nullptr &&
        options_.trace.maxInstructionRecords != 0 &&
        emitted_ >= options_.trace.maxInstructionRecords) {
      return ExecResult{SessionStop::InstructionBudget, state_.pc(), executed_,
                        "trace record budget exhausted"};
    }

    const uint64_t pc = state_.pc();
    const uint64_t executionRemaining = instructionLimit - executed_;
    uint64_t traceRemaining = executionRemaining;
    if (observer_ != nullptr &&
        options_.trace.maxInstructionRecords != 0) {
      traceRemaining = options_.trace.maxInstructionRecords - emitted_;
    }
    const std::size_t blockBudget = static_cast<std::size_t>(
        std::min<uint64_t>({executionRemaining, traceRemaining,
                            options_.maxBlockInstructions}));

    std::shared_ptr<ExecBlock> block;
    bool cached = false;
    {
      const ScopedTimer lookupTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->blockLookup};
      if (const auto found = cache_.find(pc);
          found != cache_.end() && found->second->validFor(*memory_) &&
          found->second->instructions.size() <= blockBudget) {
        block = found->second;
        cached = true;
      }
    }
    if (!cached) {
      const ScopedTimer compileTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->blockCompile};
      auto compiled = compileExecBlock(*engine_, *memory_, pc, blockBudget);
      if (!compiled) {
        return ExecResult{SessionStop::Fault, pc, executed_,
                          compiled.error().format()};
      }
      block = std::move(*compiled);
      cache_[pc] = block;
    }

    {
      const ScopedTimer bindTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->blockBind};
      if (!interpreter_) {
        interpreter_ =
            std::make_unique<il::Interpreter>(*block->lifted.function, *memory_);
      } else {
        interpreter_->rebind(*block->lifted.function);
      }
    }
    boundBlock_ = block;
    if (stateNeedsImport_) {
      const ScopedTimer importTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->stateImport};
      importState(state_, *interpreter_, rootRegisters_);
      stateNeedsImport_ = false;
    }
    il::Interpreter& interpreter = *interpreter_;

    std::vector<ConcreteValue> intrinsicArguments;
    std::string intrinsicName;

    const auto finalize = [&] {
      if (!activeValid) {
        return;
      }
      if (writeDeltaBase < active.registers.size()) {
        const ScopedTimer diffTimer{
            perfStats_ == nullptr ? nullptr : &perfStats_->registerTrace};
        std::size_t kept = writeDeltaBase;
        for (std::size_t index = writeDeltaBase; index < active.registers.size();
             ++index) {
          RegisterDelta delta = active.registers[index];
          delta.after = interpreter.readRegister(delta.reg);
          if (options_.trace.unchangedRegisterWrites ||
              delta.after != delta.before) {
            active.registers[kept++] = delta;
          }
        }
        active.registers.resize(kept);
      }
      ++executed_;
      active.sequence = executed_;
      if (observer_ != nullptr) {
        const ScopedTimer observerTimer{
            perfStats_ == nullptr ? nullptr : &perfStats_->observer};
        observer_->onInstruction(active);
        ++emitted_;
      }
      activeValid = false;
    };

    const auto hookStart = perfStats_ == nullptr
                               ? std::chrono::steady_clock::time_point{}
                               : std::chrono::steady_clock::now();
    interpreter.setInterruptHook([&](const il::Op& op) {
      return activeValid && active.pc != op.va && !block->validFor(*memory_);
    });
    interpreter.setOpHook([&](const il::Op& op) {
      if (!activeValid || active.pc != op.va) {
        finalize();
        // Assigned rather than reconstructed: a fresh InstructionRecord per
        // instruction would free and reallocate both vectors and both strings
        // tens of millions of times a trace, where reuse settles on capacity
        // after the first few and never allocates again.
        active.registers.clear();
        active.memory.clear();
        active.disassembly.clear();
        active.mnemonic.clear();
        active.sequence = 0;
        active.word = 0;
        active.length = 0;
        active.call = false;
        active.returns = false;
        active.callTarget = 0;
        active.semantics = nullptr;
        active.pc = op.va;
        activeValid = true;
        writeDeltaBase = 0;
        if (const ExecInstruction* instruction = block->instructionAt(op.va);
            instruction != nullptr) {
          active.word = instruction->word;
          active.length = instruction->length;
          active.semantics = &instruction->semantics;
          if (activeBlock != block) {
            activeBlock = block;
          }
          active.call = instruction->flow.calls;
          active.returns = instruction->flow.kind == spec::FlowKind::Return;
          active.callTarget = instruction->flow.callTargetKnown
                                  ? instruction->flow.callTarget
                                  : 0;
          if (options_.trace.disassembly) {
            active.disassembly = instruction->disassembly;
            active.mnemonic = instruction->mnemonic;
          }
          if (options_.trace.registerDeltas) {
            const ScopedTimer captureTimer{
                perfStats_ == nullptr ? nullptr : &perfStats_->registerTrace};
            // None of this instruction's ops have run, so every register still
            // holds what the instruction is about to consume. That makes this
            // one pass the source values and the destinations' prior values
            // both, and it reads each operand at the width the instruction
            // names rather than at its root's.
            const auto& operands = instruction->registerOperands;
            const std::size_t first = options_.trace.registerReads
                                          ? 0
                                          : instruction->firstWriteOperand;
            for (std::size_t index = first; index < operands.size(); ++index) {
              const ConcreteValue value =
                  interpreter.readRegister(operands[index].reg);
              active.registers.push_back(RegisterDelta{
                  operands[index].reg, value, value, operands[index].write});
            }
            writeDeltaBase = instruction->firstWriteOperand - first;
          }
        }
      }
      memory_->setInstructionContext(op.va);
    });
    memory_->setAccessObserver([&](const MemoryAccess& access) {
      if (activeValid && options_.trace.memoryAccesses) {
        MemoryAccess captured = access;
        if (!options_.trace.memoryValues) {
          captured.value = {};
        }
        if (options_.trace.memoryContextBytes != 0) {
          // A 16-byte-aligned window with a lead-in, so neighbouring accesses
          // to one structure line up in the same hexdump instead of each
          // producing its own off-by-a-few view. The ladder matters at a
          // mapping's edge: a shorter window anchored at the aligned address
          // still shows something where insisting on the lead-in shows nothing.
          const uint64_t aligned = access.address & ~uint64_t{0xF};
          const std::size_t full = options_.trace.memoryContextBytes;
          const std::array<std::pair<uint64_t, std::size_t>, 3> attempts{{
              {aligned >= 16 ? aligned - 16 : aligned, full},
              {aligned, full / 2},
              {aligned, 16},
          }};
          captured.context.clear();
          captured.contextAddress = 0;
          for (const auto& [base, length] : attempts) {
            if (length == 0) continue;
            captured.context.resize(length);
            if (memory_->readResidentBytes(base, captured.context)) {
              captured.contextAddress = base;
              break;
            }
            captured.context.clear();
          }
        }
        active.memory.push_back(std::move(captured));
      }
    });
    interpreter.setIntrinsicHook(
        [&](std::string_view name, il::Type type,
            std::span<const ConcreteValue> arguments, ConcreteValue& result) {
          // Single-threaded host model: barriers, exclusive-monitor bookkeeping,
          // and PAC tagging have no observable effect beyond the adjacent
          // load/store. Handle them in-hook so the rest of this instruction's
          // IL (e.g. ldaxr's load after reserve) still runs. Escalating to the
          // external handler would HandledContinue past the instruction and
          // skip those ops.
          if (name == "aarch64.acquire" || name == "aarch64.release" ||
              name == "aarch64.reserve" || name == "aarch64.clrex" ||
              name == "aarch64.dmb" || name == "aarch64.dsb" ||
              name == "aarch64.isb" || name == "aarch64.hint" ||
              name == "aarch64.bti" || name == "aarch64.pac.ia" ||
              name == "aarch64.pac.ib" || name == "aarch64.aut.ia" ||
              name == "aarch64.aut.ib" || name == "aarch64.dc.zva" ||
              name == "aarch64.dc.ivac" || name == "aarch64.dc.isw" ||
              name == "aarch64.dc.cvau" || name == "aarch64.dc.cvac" ||
              name == "aarch64.dc.csw" || name == "aarch64.dc.cvap" ||
              name == "aarch64.dc.cvadp" || name == "aarch64.dc.civac" ||
              name == "aarch64.dc.cisw" || name == "aarch64.ic.ialluis" ||
              name == "aarch64.ic.iallu" || name == "aarch64.ic.ivau") {
            return true;
          }
          if (name == "aarch64.store_exclusive_status") {
            // No contending observer in this interpreter: exclusive stores
            // always succeed (status 0).
            result = ConcreteValue{};
            return true;
          }
          if (name == "aarch64.cas") {
            // args: address, expected, desired. Always returns the old memory
            // value; stores desired only when it matched expected.
            if (arguments.size() < 3 || type.bits() == 0 ||
                type.bits() % 8 != 0 || type.bits() > 128) {
              return false;
            }
            const unsigned bytes = type.bits() / 8;
            auto current = memory_->read(arguments[0].lo, bytes);
            if (!current) {
              return false;
            }
            result = *current;
            if (current->lo == arguments[1].lo &&
                current->hi == arguments[1].hi) {
              if (auto written =
                      memory_->write(arguments[0].lo, bytes, arguments[2]);
                  !written) {
                return false;
              }
            }
            return true;
          }
          if (name == "aarch64.pacia" || name == "aarch64.pacib" ||
              name == "aarch64.pacda" || name == "aarch64.pacdb" ||
              name == "aarch64.autia" || name == "aarch64.autib" ||
              name == "aarch64.autda" || name == "aarch64.autdb") {
            // Identity PAC/AUT under a non-enforcing host model.
            if (arguments.size() >= 2) {
              result = arguments[1];
              return true;
            }
            return false;
          }

          intrinsicName = std::string{name};
          intrinsicArguments.assign(arguments.begin(), arguments.end());
          return false;
        });
    if (perfStats_ != nullptr) {
      perfStats_->hookInstall.add(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - hookStart)
              .count()));
    }

    il::ExecOutcome outcome;
    {
      const ScopedTimer executeTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->ilExecute};
      outcome = interpreter.runBlock(block->lifted.block);
    }
    finalize();
    {
      const ScopedTimer exportTimer{
          perfStats_ == nullptr ? nullptr : &perfStats_->stateExport};
      memory_->setAccessObserver({});
      // state_ already agrees with the interpreter here, from the import above
      // or from the previous block's export, so only what this block can write
      // has to travel back.
      exportState(interpreter, state_, block->writtenRoots);
    }

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
        request.dirty = collectDirty();
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
        request.dirty = collectDirty();
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
    request.dirty = collectDirty();

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
