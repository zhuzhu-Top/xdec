// Turning bytes into IL.
//
// Three operations, in increasing cost, all driven by the same compiled spec:
//
//   decode    bytes -> which rule matched, and its field values
//   probe     that rule's control-flow effect, without building any IL
//   elaborate that rule's full semantics, as IL appended to a block
//
// Probing exists because of a chicken and egg problem: a branch names a target
// address, but the IL wants a block, and blocks are not known until every
// branch in the function has been found. So the driver probes each instruction
// to discover the control-flow graph, creates the blocks, and only then
// elaborates. Probing runs the same bytecode as elaboration with IL
// construction switched off, so the two can never disagree about where a branch
// goes.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "xdec/il/function.h"
#include "xdec/spec/program.h"
#include "xdec/support/result.h"

namespace xdec::spec {

inline constexpr unsigned kMaxFields = 16;

/// What a decoded instruction turned out to be.
struct DecodedInsn {
  bool valid = false;
  /// Index into the program's instruction table.
  uint32_t instruction = 0;
  uint64_t address = 0;
  /// The raw instruction word.
  uint64_t word = 0;
  unsigned length = 0;
  /// Field values, in the order the rule declares them.
  uint64_t fields[kMaxFields] = {};
  unsigned fieldCount = 0;

  [[nodiscard]] uint64_t next() const noexcept { return address + length; }
};

enum class FlowKind : uint8_t {
  /// Execution continues at the next instruction.
  Fallthrough,
  /// An unconditional transfer to a known address.
  Branch,
  /// Two known successors.
  CondBranch,
  /// A computed target. Resolving these is a later pass's job, and pretending
  /// otherwise is how a deobfuscator ends up with a silently truncated CFG.
  IndirectBranch,
  Return,
  Unreachable,
  /// The rule could not be decoded or lifted.
  Unknown,
};

[[nodiscard]] std::string_view toString(FlowKind kind) noexcept;

struct InsnFlow {
  FlowKind kind = FlowKind::Fallthrough;
  uint64_t target = 0;
  uint64_t fallthrough = 0;
  /// A call was made. Independent of `kind`, because control usually returns.
  bool calls = false;
  /// The call target, when it is known.
  bool callTargetKnown = false;
  uint64_t callTarget = 0;

  /// Whether the instruction ends a basic block.
  [[nodiscard]] bool terminates() const noexcept { return kind != FlowKind::Fallthrough; }
};

/// Where elaboration puts the IL it builds.
struct LiftSite {
  il::Function* function = nullptr;
  il::BlockId block;
  uint64_t address = 0;
  /// Maps a branch target address to the block that starts there. An invalid
  /// result means the address is outside the function, which the engine reports
  /// rather than papering over.
  std::function<il::BlockId(uint64_t)> blockAt;
};

class SpecEngine {
 public:
  /// Takes ownership so that the program outlives every lift that uses it.
  explicit SpecEngine(std::unique_ptr<SpecProgram> program);

  [[nodiscard]] const SpecProgram& program() const noexcept { return *program_; }

  /// Reads one instruction. Never fails: an unrecognised word is reported as an
  /// invalid DecodedInsn, because a decompiler meets undecodable bytes routinely
  /// and stopping would be worse than saying so.
  [[nodiscard]] DecodedInsn decode(std::span<const std::byte> bytes, uint64_t address) const;

  /// The control-flow effect of an already decoded instruction.
  [[nodiscard]] InsnFlow probe(const DecodedInsn& insn) const;

  /// Renders the disassembly text.
  [[nodiscard]] std::string disassemble(const DecodedInsn& insn) const;

  /// Builds the IL for one instruction. Every op it appends carries the
  /// instruction's address, which is the provenance the rest of the pipeline
  /// depends on.
  [[nodiscard]] Result<void> elaborate(const DecodedInsn& insn, const LiftSite& site) const;

 private:
  [[nodiscard]] std::string renderOperand(AsmStyle style, uint64_t value, uint64_t styleArgument,
                                          const DecodedInsn& insn) const;

  std::unique_ptr<SpecProgram> program_;
};

/// Loads a spec from source, compiling it. For tools; a release path would load
/// a blob instead.
[[nodiscard]] Result<std::unique_ptr<SpecEngine>> loadSpecSource(std::string_view text,
                                                                 std::string_view name);

/// Loads a spec from a file, following its includes.
[[nodiscard]] Result<std::unique_ptr<SpecEngine>> loadSpecFile(const std::filesystem::path& path);

}  // namespace xdec::spec
