// Semantic checking for the instruction semantics DSL.
//
// The parser accepts anything syntactically well formed; this is the single
// place that decides what is meaningful. It answers three questions:
//
//   1. Does every name resolve, and does every call match its signature? The
//      signatures are the IL's own op signatures, so a spec that type-checks
//      cannot construct malformed IL.
//   2. Do widths agree? Widths are symbolic, because one rule covers both the
//      32- and 64-bit forms of an instruction, so this is a symbolic proof
//      rather than an integer comparison.
//   3. Can two encodings match the same instruction word?
//
// Everything it cannot prove is reported. A spec that "probably works" is worse
// than one that fails to compile: the failure mode of a wrong semantic rule is a
// plausible wrong decompilation, discovered much later and attributed elsewhere.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "xdec/il/register_file.h"
#include "xdec/spec/ast.h"
#include "xdec/spec/encoding.h"
#include "xdec/spec/symint.h"
#include "xdec/support/result.h"

namespace xdec::spec {

struct CheckReport {
  std::vector<Diag> errors;
  std::vector<Diag> warnings;

  [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
  [[nodiscard]] std::string format() const;
};

/// Where a register file's registers landed in the IL register file. Elements of
/// one file and of one view are contiguous, so the elaborator can index rather
/// than look up by name.
struct RegFileBinding {
  std::string name;
  il::RegId base;
  unsigned count = 0;
  unsigned bits = 0;
  /// Base register of each declared view, in declaration order.
  std::vector<il::RegId> viewBase;
  std::vector<unsigned> viewBits;
  std::vector<std::string> viewNames;
  il::RegClass role = il::RegClass::General;
};

/// One decoded field's position in the instruction word.
struct FieldBinding {
  std::string name;
  /// Low bit within the instruction word.
  unsigned shift = 0;
  unsigned bits = 0;
};

struct CheckedInsn {
  /// Index into the module's instruction list.
  uint32_t index = 0;
  std::string name;
  std::vector<FieldBinding> fields;
};

/// A module that has passed checking, plus everything derived from it that the
/// spec compiler and the runtime engine need.
struct CheckedModule {
  /// Borrowed: the caller owns the AST and must keep it alive.
  const Module* ast = nullptr;
  /// Built from the arch declaration, ready for the IL to use.
  il::RegisterFile registers;
  std::vector<RegFileBinding> regFiles;
  /// Named registers, by their spec name.
  std::unordered_map<std::string, il::RegId> namedRegs;
  std::vector<CheckedInsn> instructions;
  std::vector<EncodingPattern> patterns;
  DecisionTree decoder;

  [[nodiscard]] const RegFileBinding* findRegFile(std::string_view name) const;
  [[nodiscard]] const CheckedInsn* findInsn(std::string_view name) const;
};

/// Checks a parsed module. Returns the checked form and a report; the report may
/// carry warnings even on success. On error the checked module is still
/// returned, partially populated, so that tooling can show what was understood.
struct CheckResult {
  std::unique_ptr<CheckedModule> module;
  CheckReport report;
};

[[nodiscard]] CheckResult check(const Module& module);

/// Convenience wrapper for callers that only want a pass or fail.
[[nodiscard]] Result<std::unique_ptr<CheckedModule>> checkOrFail(const Module& module);

/// Names the DSL provides. Exposed for the reference documentation test, which
/// checks that every builtin is documented.
[[nodiscard]] const std::vector<std::string>& builtinNames();

}  // namespace xdec::spec
