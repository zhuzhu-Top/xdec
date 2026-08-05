// Executing the IL concretely.
//
// The interpreter is the reference for what the lifted IL means, which makes it
// one half of every semantic oracle: differential testing runs a lifted block
// here and the same instructions in Unicorn with identical state and compares,
// and a later constant-folding pass proves itself by showing the interpreter
// computes the same values before and after the fold.
//
// It interprets one basic block at a time rather than following control flow.
// Whole-function interpretation needs an answer for calls, and any answer
// invented here -- a stack discipline, an ABI -- would be one the tests then
// have to peel away. Block-at-a-time is also exactly the granularity of the
// Unicorn differential, since a basic block is where a lifter's semantics live.
//
// Two simplifications are deliberate. Flag bundles are materialised to their
// four bits the moment a FlagDef is evaluated: laziness is a property of the
// IR as a data structure, not of its meaning, and executing eagerly is what
// lets the Unicorn run compare NZCV directly. And division by zero produces
// zero rather than trapping, because that is what AArch64 hardware does and the
// IL ops inherit the semantics of the target they were lifted from.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/support/result.h"

namespace xdec::il {

/// A concrete value: up to 128 bits, little-endian in two words.
///
/// 128 is the widest value the AArch64 lifter produces (a q register, the
/// concatenation inside `extr`). The IL type system allows wider, and the
/// interpreter reports an error rather than truncate if it ever meets one.
struct ConcreteValue {
  uint64_t lo = 0;
  uint64_t hi = 0;

  friend bool operator==(ConcreteValue lhs, ConcreteValue rhs) noexcept {
    return lhs.lo == rhs.lo && lhs.hi == rhs.hi;
  }
};

/// A half-open byte range, [address, address + size).
struct WrittenRange {
  uint64_t address = 0;
  uint64_t size = 0;
};

/// Sparse byte-addressed memory in two layers.
///
/// The seed layer holds bytes that are there before execution: the binary
/// image, the stack's initial contents. The delta layer holds everything
/// execution or per-run setup writes. Reads see delta first, then seed, then
/// fault. This split is what makes running the same block a thousand times
/// against random states cheap: clearing the run is `clearDelta()`, never a
/// re-seed.
///
/// Store ops record their bytes in a write set, so a differential run can
/// compare exactly what execution touched instead of diffing whole pages.
/// Setup writes through `seed` and `fillDelta` do not pollute that set.
class ExecMemory {
 public:
  static constexpr unsigned kPageBits = 12;
  static constexpr uint64_t kPageSize = uint64_t{1} << kPageBits;

  /// Seeds bytes that persist across `clearDelta`. Seeding twice replaces.
  void seed(uint64_t address, std::span<const std::byte> bytes);

  /// Writes into the delta layer without recording a write-set entry. This is
  /// how a workload supplies per-run initial memory.
  void fillDelta(uint64_t address, std::span<const std::byte> bytes);

  [[nodiscard]] bool mapped(uint64_t address, uint64_t size) const;

  /// Up to 16 bytes, little-endian, zero-extended into the value.
  [[nodiscard]] Result<ConcreteValue> read(uint64_t address, unsigned bytes) const;
  /// Up to 16 bytes taken from the value's low bits, little-endian. Recorded in
  /// the write set.
  [[nodiscard]] Result<void> write(uint64_t address, unsigned bytes, ConcreteValue value);

  /// Coalesced ranges the Store ops have written since the last `clearDelta`.
  [[nodiscard]] std::vector<WrittenRange> writtenRanges() const;
  void clearDelta();

 private:
  struct Page {
    std::array<std::byte, kPageSize> bytes{};
  };

  [[nodiscard]] const Page* pageAt(uint64_t pageBase) const;
  [[nodiscard]] Page* deltaPage(uint64_t pageBase);

  std::unordered_map<uint64_t, Page> seed_;
  std::unordered_map<uint64_t, Page> delta_;
  std::vector<uint64_t> writtenBytes_;
};

enum class ExecStop : uint8_t {
  /// Unconditional edge to `target`.
  Branch,
  /// `condition` says which of `target` / `fallthrough` is next.
  CondBranch,
  /// Computed edge; `target` is the evaluated destination.
  IndirectBranch,
  /// A call to `target`. Execution stops even though control would return:
  /// what the callee does is not this block's business.
  Call,
  Return,
  Unreachable,
  /// An instruction the lifter could not model, by name in `detail`.
  Unimplemented,
  /// An intrinsic the hook declined, by name in `detail`.
  Intrinsic,
  /// Something execution cannot continue past: a memory fault, a value used
  /// before definition, an unsupported width. `detail` says what.
  Error,
};

[[nodiscard]] std::string_view toString(ExecStop stop) noexcept;

struct ExecOutcome {
  ExecStop stop = ExecStop::Error;
  /// The machine address of the op that ended execution.
  uint64_t va = 0;
  uint64_t target = 0;
  uint64_t fallthrough = 0;
  bool condition = false;
  std::string detail;
};

class Interpreter {
 public:
  /// Decision delegate for intrinsics. Return true to model the operation:
  /// execution continues, and for a non-void result type `result` must be set.
  /// Return false and execution stops with ExecStop::Intrinsic, which is the
  /// honest answer for an effect nobody has modelled.
  using IntrinsicHook = std::function<bool(std::string_view name, Type resultType,
                                           std::span<const ConcreteValue> arguments,
                                           ConcreteValue& result)>;

  /// With `memory` null the interpreter owns a fresh, empty memory. Passing
  /// one shares it: the interpreter reads and writes through it but does not
  /// own it, which is how a driver seeds the image once and runs many blocks
  /// against it.
  explicit Interpreter(const Function& function, ExecMemory* memory = nullptr);

  [[nodiscard]] const Function& function() const noexcept { return *function_; }
  [[nodiscard]] ExecMemory& memory() noexcept { return *memory_; }

  /// Register access through the register file's views: writing `w0`
  /// zero-extends into `x0` because the view declares it, and writing a
  /// zero-class register discards the value, because the hardware does.
  void writeRegister(RegId reg, ConcreteValue value);
  [[nodiscard]] ConcreteValue readRegister(RegId reg) const;

  void setIntrinsicHook(IntrinsicHook hook) { hook_ = std::move(hook); }

  /// Zeroes registers and defined values. Memory survives, delta included;
  /// clearing it is a separate decision (`ExecMemory::clearDelta`).
  void resetState();

  /// Executes one block from its first op through its terminator.
  [[nodiscard]] ExecOutcome runBlock(BlockId block);

 private:
  [[nodiscard]] Result<ConcreteValue> eval(ExprId id, unsigned depth);
  [[nodiscard]] Result<ConcreteValue> evalFlagsOperand(ExprId id, unsigned depth);
  [[nodiscard]] ExecOutcome fail(std::string message, uint64_t va);

  const Function* function_;
  ExecMemory* memory_;
  std::unique_ptr<ExecMemory> ownedMemory_;
  IntrinsicHook hook_;

  /// One cell per root register. Sub-register reads and writes resolve through
  /// the view chain; flags registers keep their four materialised bits in `lo`.
  std::vector<ConcreteValue> registers_;
  std::vector<ConcreteValue> values_;
  std::vector<bool> defined_;
};

}  // namespace xdec::il
