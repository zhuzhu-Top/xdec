// resolve-indirect: what happens to a computed branch it cannot answer.
//
// Resolution itself is exercised end-to-end by the driver tests and the eval
// corpus, which have real code to resolve. What those cannot cover is the
// failure path, because they are built out of branches that do resolve. These
// tests supply one that provably cannot -- a jump through a register nothing
// in the function defines -- and pin the two things the pass is allowed to do
// with it.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/pass.h"
#include "xdec/passes/resolve_indirect.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;

namespace {

/// Enough of an address space for the pass to accept the pipeline; nothing in
/// these tests is meant to be read successfully.
xdec::ByteReader emptyImage() {
  return [](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    (void)out;
    return xdec::err(xdec::DiagCode::Internal, "unmapped read at {:#x}", va);
  };
}

/// One block, ending in `br x0` with x0 the caller's -- no table, no value set,
/// nothing to resolve, which is the situation under test.
Function unresolvable() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  function.appendIndirectBranch(entry, 0x1004,
                                function.entryReg(function.registers().find("x0")));
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);
  return function;
}

bool run(Function& function, bool seal) {
  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(emptyImage());
  context.setSealUnresolvedBranches(seal);
  auto result = pass->run(context);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);
  return *result;
}

[[nodiscard]] const il::Op& terminatorOf(const Function& function, BlockId block) {
  return function.op(function.block(block).ops.back());
}

}  // namespace

TEST_CASE("a branch that cannot be resolved is left for the verifier to report",
          "[passes][resolve-indirect]") {
  Function function = unresolvable();
  const BlockId entry = function.entryBlock();

  CHECK_FALSE(run(function, /*seal=*/false));
  CHECK(terminatorOf(function, entry).code == OpCode::IndirectBranch);
  // The hole is the point: everything above Resolved would be reasoning about a
  // function it cannot see all of, so the gate stops the run by default.
  const il::VerifyReport report = il::verify(function, Maturity::Resolved);
  CHECK_FALSE(report.ok());
}

TEST_CASE("sealing turns an unanswerable branch into an opaque terminator",
          "[passes][resolve-indirect]") {
  Function function = unresolvable();
  const BlockId entry = function.entryBlock();

  CHECK(run(function, /*seal=*/true));
  const il::Op& terminator = terminatorOf(function, entry);
  REQUIRE(terminator.code == OpCode::Unimplemented);
  // The address, so the reader can go look at it, and the expression, so they
  // know what kind of hole it is before they do.
  const std::string_view name = function.nameOf(terminator.payload);
  CHECK(name.find("unresolved indirect branch") != std::string_view::npos);
  CHECK(name.find("0x1004") != std::string_view::npos);
  CHECK(name.find("x0") != std::string_view::npos);
  // Legal IL at Resolved, which is the whole reason to seal rather than to
  // relax the gate: nothing downstream has to learn a new exception.
  const il::VerifyReport report = il::verify(function, Maturity::Resolved);
  for (const xdec::Diag& diag : report.errors) {
    INFO(diag.format());
  }
  CHECK(report.ok());
}

TEST_CASE("sealing leaves unreachable blocks alone", "[passes][resolve-indirect]") {
  Function function = unresolvable();
  // Swept up behind the entry, branched to by nothing. The verifier already
  // exempts these, and sealing them would mark holes in code that never runs.
  const BlockId stray = function.createBlock(0x2000);
  function.appendIndirectBranch(stray, 0x2000,
                                function.entryReg(function.registers().find("x1")));
  function.rebuildEdges();

  CHECK(run(function, /*seal=*/true));
  CHECK(terminatorOf(function, stray).code == OpCode::IndirectBranch);
}
