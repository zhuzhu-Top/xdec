// Unified RuntimeOracle over ExecSession (dynamic emulation & dirty-range
// extraction for bootstrap unpacking) and Tracedb (observed branch targets).
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "xdec/devirt/memory.h"
#include "xdec/exec/memory.h"
#include "xdec/exec/session.h"
#include "xdec/spec/engine.h"
#include "xdec/storage/tracedb.h"
#include "xdec/support/result.h"

namespace xdec::devirt {

struct BootstrapPatch {
  uint64_t address = 0;
  std::vector<uint8_t> bytes;
};

struct BootstrapResult {
  exec::ExecResult execResult;
  uint64_t returnValue = 0;
  std::vector<BootstrapPatch> dirtyPatches;
};

class RuntimeOracle {
 public:
  /// Runs a native initialization / decryption routine at `entryVa` via
  /// `ExecSession` over `baseMemory` and captures all dirty byte ranges,
  /// optionally committing them back into `applyToLayered`.
  [[nodiscard]] static Result<BootstrapResult> runBootstrap(
      const spec::SpecEngine& engine, const LayeredByteReader& baseMemory,
      uint64_t entryVa, std::span<const uint64_t> args, uint64_t stackTop,
      uint64_t returnLr, uint64_t maxInstructions = 500'000,
      LayeredByteReader* applyToLayered = nullptr, bool markImmutable = true);

  /// Queries a Tracedb v5/v6 directory for unique observed branch/call targets
  /// originating from `sourcePc`.
  [[nodiscard]] static Result<std::vector<uint64_t>> observedTargets(
      const std::filesystem::path& tracedbDir, uint64_t sourcePc);
};

}  // namespace xdec::devirt
