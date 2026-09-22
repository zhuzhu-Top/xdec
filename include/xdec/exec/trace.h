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

struct InstructionSemantics;

enum class BoundaryKind : uint8_t {
  DirectCall,
  IndirectCall,
  Syscall,
  Intrinsic,
  PageRequest,
};

[[nodiscard]] std::string_view toString(BoundaryKind kind) noexcept;

/// One register an instruction touched, at the granularity the instruction
/// named it: a 32-bit write reports `w0`, not `x0`. Reads carry the value the
/// instruction consumed, with `before` and `after` equal.
struct RegisterDelta {
  il::RegId reg;
  ConcreteValue before;
  ConcreteValue after;
  bool write = true;
};

struct InstructionRecord {
  uint64_t sequence = 0;
  uint64_t pc = 0;
  uint64_t word = 0;
  unsigned length = 0;
  std::string disassembly;
  /// The compiled instruction's precomputed ExecInstruction::mnemonic, so
  /// observers that classify by opcode don't re-parse disassembly per
  /// occurrence of a hot loop's body.
  std::string mnemonic;
  std::vector<RegisterDelta> registers;
  std::vector<MemoryAccess> memory;
  /// What the lifter says this instruction computes, so an observer that needs
  /// more than the mnemonic -- which constant an `and` masks with, which way a
  /// variable shift goes -- reads xdec's own answer instead of decoding the
  /// word a second time and drifting from the xspec rules.
  ///
  /// Borrowed from the compiled block, which the session keeps alive for as
  /// long as the record is live, and shared by every execution of the same
  /// instruction: copying it per record would allocate on the hottest path
  /// there is. Null when the block did not compile the instruction.
  const InstructionSemantics* semantics = nullptr;
  /// Control-flow shape, taken from the lifted instruction rather than guessed
  /// from the mnemonic, so an observer can build a call tree without decoding
  /// anything itself.
  bool call = false;
  bool returns = false;
  /// The call target where the encoding names one. Zero for an indirect call,
  /// whose real target is simply the next instruction executed.
  uint64_t callTarget = 0;
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
  /// Whether source operands are recorded alongside destinations. Backward
  /// data flow needs them -- without a read there is no edge from a value to
  /// the instruction that consumed it -- but they roughly double the register
  /// records a trace produces, so a consumer that only wants results can
  /// decline them.
  bool registerReads = true;
  /// Whether a destination is recorded even when the instruction wrote the
  /// value it already held. Off, the trace shows only changes; on, it shows
  /// every write the encoding performs, which is what a consumer reconciling
  /// against a disassembler expects.
  bool unchangedRegisterWrites = true;
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
