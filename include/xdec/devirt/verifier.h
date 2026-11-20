// Differential bit-exact verifier comparing baseline IL (or expected ground
// truth vectors) against optimized/devirtualized IL via `il::Interpreter`.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xdec/analysis/abi.h"
#include "xdec/devirt/memory.h"
#include "xdec/il/function.h"
#include "xdec/il/interp.h"
#include "xdec/support/result.h"

namespace xdec::devirt {

struct DiffVector {
  std::vector<uint64_t> args;
  uint64_t stackTop = 0;
  std::optional<uint64_t> expectedReturn;
};

struct DiffReport {
  std::size_t vectorsChecked = 0;
  bool matched = false;
  std::optional<std::size_t> failedVector;
  uint64_t expectedValue = 0;
  uint64_t actualValue = 0;
  std::string detail;
};

class DiffVerifier {
 public:
  /// Runs `baseline` and `candidate` over all `vectors` using
  /// `il::Interpreter::runFunction`, failing on the first 1-bit divergence or
  /// mismatch against `DiffVector::expectedReturn`.
  [[nodiscard]] static Result<DiffReport> verify(
      const il::Function& baseline, const il::Function& candidate,
      std::span<const DiffVector> vectors,
      const LayeredByteReader* memory = nullptr,
      const analysis::AbiSpec* abi = nullptr,
      uint64_t returnSentinelVa = 0);

  /// Verifies a single `il::Function` directly against vectors that carry
  /// `expectedReturn`.
  [[nodiscard]] static Result<DiffReport> verifyAgainstExpected(
      const il::Function& fn, std::span<const DiffVector> vectors,
      const LayeredByteReader* memory = nullptr,
      const analysis::AbiSpec* abi = nullptr,
      uint64_t returnSentinelVa = 0);
};

}  // namespace xdec::devirt
