// Mid-level decompilation entry point: bypasses native machine-code lifting
// and drives an externally constructed `il::Function` (e.g. from a custom
// virtual ISA or partial evaluator) through the standard SSA -> Optimize ->
// Vars -> Structuring -> Emit pipeline.
#pragma once

#include <memory>
#include <optional>

#include "xdec/analysis/abi.h"
#include "xdec/decompile/emit.h"
#include "xdec/devirt/memory.h"
#include "xdec/il/function.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::decompile {

struct DecompileFromILOptions {
  std::optional<analysis::AbiSpec> abi;
  const devirt::LayeredByteReader* layeredMemory = nullptr;
  std::optional<ByteReader> memory;
  MemoryFacts memoryFacts;
  const devirt::MemoryRegionMap* regions = nullptr;
  const types::TypeDatabase* types = nullptr;
  const types::SyscallTable* syscalls = nullptr;
  pass::NameAt names;
  emit::COptions emit;
  emit::StructureOptions structure;
  const binary::TargetProfile* profile = nullptr;
  bool sealUnresolvedBranches = false;
  bool computeEmitRedundancy = false;
  bool computeObfuscationProfile = false;
  bool captureAst = false;
  pass::Observer* observer = nullptr;
};

[[nodiscard]] Result<DecompileToCResult> decompileFromIL(
    std::unique_ptr<il::Function> function, pass::Registry& registry,
    const DecompileFromILOptions& options = {});

}  // namespace xdec::decompile
