// IL invariant checking.
//
// The verifier runs after every pass. That is not paranoia: the failure mode of
// a decompiler pass is not a crash but plausible wrong output, and by the time
// wrongness is visible in emitted C the responsible pass is twenty stages back.
// Checking invariants at each step converts a debugging session into an error
// message naming the pass and the address.
//
// It reports every problem it finds rather than stopping at the first, because
// one broken assumption usually manifests in several places and seeing them
// together identifies the cause faster.
#pragma once

#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/support/result.h"

namespace xdec::il {

struct VerifyReport {
  /// Violations of an invariant the level guarantees.
  std::vector<Diag> errors;
  /// Structurally legal but suspicious, such as an unreachable block.
  std::vector<Diag> warnings;

  [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
  [[nodiscard]] std::string format() const;
};

/// Checks the invariants that the function's own maturity level promises.
[[nodiscard]] VerifyReport verify(const Function& function);

/// Checks against an explicit level, for a pass validating its own output before
/// advancing the recorded maturity.
[[nodiscard]] VerifyReport verify(const Function& function, Maturity level);

/// Result-returning wrapper for use with XDEC_TRY_VOID.
[[nodiscard]] Result<void> verifyOrFail(const Function& function);

}  // namespace xdec::il
