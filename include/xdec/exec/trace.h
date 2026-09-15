// Logical execution events. Persistence formats are adapters, not this ABI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/exec/memory.h"
#include "xdec/exec/state.h"

namespace xdec::exec {

enum class BoundaryKind : uint8_t {
  DirectCall,
  IndirectCall,
  Syscall,
  Intrinsic,
  PageRequest,
};

[[nodiscard]] std::string_view toString(BoundaryKind kind) noexcept;

struct RegisterDelta {
  il::RegId reg;
  ConcreteValue before;
  ConcreteValue after;
};

struct InstructionRecord {
  uint64_t sequence = 0;
  uint64_t pc = 0;
  uint64_t word = 0;
  unsigned length = 0;
  std::string disassembly;
  std::vector<RegisterDelta> registers;
  std::vector<MemoryAccess> memory;
};

struct BoundaryRecord {
  uint64_t pc = 0;
  uint64_t returnPc = 0;
  uint64_t target = 0;
  BoundaryKind kind = BoundaryKind::Intrinsic;
  std::string name;
};

enum class ExternalAction : uint8_t {
  /// Continue at the boundary's return PC.
  HandledContinue,
  /// A call model completed and returns to the boundary's return PC.
  ReturnFromCall,
  /// Continue at an explicitly supplied PC.
  Redirect,
  /// Keep the session suspended.
  Stop,
};

struct BoundaryEffectRecord {
  BoundaryRecord boundary;
  /// Empty when the handler or its returned patch failed.
  std::optional<ExternalAction> action;
  uint64_t nextPc = 0;
  StatePatch state;
  std::vector<MemoryPatch> memory;
  bool memoryAlreadySynchronized = false;
  std::string error;
};

struct TracePolicy {
  bool disassembly = true;
  bool registerDeltas = true;
  bool memoryAccesses = true;
  bool memoryValues = true;
  /// Total bytes captured around each access; zero disables context.
  std::size_t memoryContextBytes = 0;
  bool externalEffects = true;
  /// Maximum instruction records and executed instructions; boundary and
  /// effect records attached to the last instruction are not counted.
  std::size_t maxInstructionRecords = 0;  // zero is unlimited
};

class IExecObserver {
 public:
  virtual ~IExecObserver() = default;
  virtual void onInstruction(const InstructionRecord& record) = 0;
  virtual void onBoundary(const BoundaryRecord& record) { (void)record; }
  virtual void onBoundaryEffect(const BoundaryEffectRecord& record) {
    (void)record;
  }
};

class CallbackTraceSink final : public IExecObserver {
 public:
  using InstructionCallback = std::function<void(const InstructionRecord&)>;
  using BoundaryCallback = std::function<void(const BoundaryRecord&)>;
  using EffectCallback = std::function<void(const BoundaryEffectRecord&)>;

  explicit CallbackTraceSink(InstructionCallback instruction,
                             BoundaryCallback boundary = {},
                             EffectCallback effect = {})
      : instruction_(std::move(instruction)),
        boundary_(std::move(boundary)),
        effect_(std::move(effect)) {}

  void onInstruction(const InstructionRecord& record) override;
  void onBoundary(const BoundaryRecord& record) override;
  void onBoundaryEffect(const BoundaryEffectRecord& record) override;

 private:
  InstructionCallback instruction_;
  BoundaryCallback boundary_;
  EffectCallback effect_;
};

class VectorTraceSink final : public IExecObserver {
 public:
  void onInstruction(const InstructionRecord& record) override {
    instructions.push_back(record);
  }
  void onBoundary(const BoundaryRecord& record) override {
    boundaries.push_back(record);
  }
  void onBoundaryEffect(const BoundaryEffectRecord& record) override {
    effects.push_back(record);
  }

  std::vector<InstructionRecord> instructions;
  std::vector<BoundaryRecord> boundaries;
  std::vector<BoundaryEffectRecord> effects;
};

class TextTraceSink final : public IExecObserver {
 public:
  explicit TextTraceSink(std::ostream& output) : output_(&output) {}

  void onInstruction(const InstructionRecord& record) override;
  void onBoundary(const BoundaryRecord& record) override;
  void onBoundaryEffect(const BoundaryEffectRecord& record) override;

 private:
  std::ostream* output_;
};

}  // namespace xdec::exec
