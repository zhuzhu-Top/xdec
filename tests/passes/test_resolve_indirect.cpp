// resolve-indirect: what happens to a computed branch it cannot answer.
//
// Resolution itself is exercised end-to-end by the driver tests and the eval
// corpus, which have real code to resolve. What those cannot cover is the
// failure path, because they are built out of branches that do resolve. These
// tests supply one that provably cannot -- a jump through a register nothing
// in the function defines -- and pin the two things the pass is allowed to do
// with it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "il/il_test_support.h"
#include "xdec/analysis/entry_reg.h"
#include "xdec/il/function.h"
#include "xdec/il/verify.h"
#include "xdec/pass/pass.h"
#include "xdec/passes/resolve_indirect.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::Type;

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

bool run(Function& function, bool seal, xdec::ByteReader image = emptyImage(),
        const xdec::analysis::EntryRegFacts* entryRegs = nullptr) {
  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(std::move(image));
  context.setSealUnresolvedBranches(seal);
  context.setEntryRegFacts(entryRegs);
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

namespace {

/// A table's worth of qwords, for the fake image below to serve.
struct FakeTableImage {
  std::map<uint64_t, uint64_t> qwords;

  [[nodiscard]] xdec::ByteReader reader() const {
    return [this](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
      const auto found = qwords.find(va);
      if (found == qwords.end() || out.size() > 8) {
        return xdec::err(xdec::DiagCode::UnmappedAddress, "not in the fake image");
      }
      for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = static_cast<std::byte>(found->second >> (index * 8));
      }
      return {};
    };
  }
};

}  // namespace

TEST_CASE(
    "a table entry the index's own value set skips is never claimed, even inside "
    "the structural bound",
    "[passes][resolve-indirect]") {
  // index = cond ? 0 : 2 -- both arms plain constants, condition opaque (an
  // unconstrained entry register), so ImageEval's evaluator (which unions a
  // select's arms when the condition does not collapse to one of them -- see
  // analysis/image_eval.cpp) reads this as exactly {0, 2}: never 1, whatever
  // cond turns out to be. The structural bound, by contrast, sees only two
  // plain constants and answers max(0, 2) = 2, which is wide enough to admit
  // 1 -- the case this test exists to catch: reading the table 0..bound wide
  // would ask for entry 1, which this fake image deliberately leaves with no
  // block behind it.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const BlockId case0 = function.createBlock(0x2000);
  const BlockId case2 = function.createBlock(0x2020);

  const ExprId cond = function.binary(
      ExprOp::CmpEq, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0x1234));
  const ExprId index =
      function.select(cond, function.constant(Type::integer(64), 0),
                      function.constant(Type::integer(64), 2));
  const ExprId address = function.binary(
      ExprOp::Add, function.constant(Type::integer(64), 0x30000),
      function.binary(ExprOp::Shl, index, function.constant(Type::integer(64), 3)));
  const ExprId target =
      function.valueRef(function.appendLoad(entry, 0x1000, Type::integer(64), address));
  function.appendIndirectBranch(entry, 0x1004, target);
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeTableImage image;
  image.qwords[0x30000] = 0x2000;  // index 0 -> case0
  // 0x30008 (index 1) deliberately absent: claiming it would fail this test,
  // either by reporting it as a missing discovery (the branch would not
  // resolve this round) or, worse, by resolving to garbage.
  image.qwords[0x30010] = 0x2020;  // index 2 -> case2
  // isCode() falls back to plain readability with no MemoryFacts wired in
  // (see resolve_indirect.cpp), so the two branch targets need a byte behind
  // them too, distinct from the table's own bytes above.
  image.qwords[0x2000] = 0;
  image.qwords[0x2020] = 0;

  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(image.reader());
  auto result = pass->run(context);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);
  CHECK(*result);

  const auto targets = function.targets(terminatorOf(function, entry));
  REQUIRE(targets.size() == 2);
  CHECK(std::find(targets.begin(), targets.end(), case0) != targets.end());
  CHECK(std::find(targets.begin(), targets.end(), case2) != targets.end());
}

TEST_CASE("an entry pointing back past the branch's own table read is not a target",
          "[passes][resolve-indirect]") {
  // An offset table anchored at the dispatcher itself, with one entry naming
  // the branch instruction. Taking it would jump straight past the load and
  // add the anchor a second time -- to an address the first pass through
  // already finished computing -- so the entry cannot be what it looks like.
  // Left in, it would resolve as a self edge, and the phi that edge puts in
  // front of the load is what stops matchJumpTable seeing a table here at all.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const BlockId handler = function.createBlock(0x2000);

  const ExprId cond = function.binary(
      ExprOp::CmpEq, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0x1234));
  const ExprId index =
      function.select(cond, function.constant(Type::integer(64), 0),
                      function.constant(Type::integer(64), 1));
  const ExprId address = function.binary(
      ExprOp::Add, function.constant(Type::integer(64), 0x30000),
      function.binary(ExprOp::Shl, index, function.constant(Type::integer(64), 2)));
  const ExprId offset =
      function.valueRef(function.appendLoad(entry, 0x1000, Type::integer(32), address));
  const ExprId target =
      function.binary(ExprOp::Add, function.constant(Type::integer(64), 0x1000),
                      function.cast(ExprOp::SExt, Type::integer(64), offset));
  function.appendIndirectBranch(entry, 0x1004, target);
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeTableImage image;
  image.qwords[0x30000] = 0x1000;  // index 0 -> anchor + 0x1000 = handler
  image.qwords[0x30004] = 0x0004;  // index 1 -> anchor + 4 = the branch itself
  image.qwords[0x2000] = 0;

  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(image.reader());
  auto result = pass->run(context);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);
  CHECK(*result);

  const auto targets = function.targets(terminatorOf(function, entry));
  REQUIRE(targets.size() == 1);
  CHECK(targets[0] == handler);
}

TEST_CASE("an entry pointing back at the read itself stays a target",
          "[passes][resolve-indirect]") {
  // The same table, except the second entry names the dispatcher's own first
  // instruction rather than the branch. That one re-reads the table with
  // whatever index it computes next time round -- an ordinary dispatch loop,
  // and a real edge, so nothing above may drop it.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const BlockId handler = function.createBlock(0x2000);

  const ExprId cond = function.binary(
      ExprOp::CmpEq, function.entryReg(function.registers().find("x0")),
      function.constant(Type::integer(64), 0x1234));
  const ExprId index =
      function.select(cond, function.constant(Type::integer(64), 0),
                      function.constant(Type::integer(64), 1));
  const ExprId address = function.binary(
      ExprOp::Add, function.constant(Type::integer(64), 0x30000),
      function.binary(ExprOp::Shl, index, function.constant(Type::integer(64), 2)));
  const ExprId offset =
      function.valueRef(function.appendLoad(entry, 0x1000, Type::integer(32), address));
  const ExprId target =
      function.binary(ExprOp::Add, function.constant(Type::integer(64), 0x1000),
                      function.cast(ExprOp::SExt, Type::integer(64), offset));
  function.appendIndirectBranch(entry, 0x1004, target);
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeTableImage image;
  image.qwords[0x30000] = 0x1000;  // index 0 -> handler
  image.qwords[0x30004] = 0x0000;  // index 1 -> anchor + 0 = the read itself
  image.qwords[0x2000] = 0;
  image.qwords[0x1000] = 0;

  const auto pass = xdec::passes::makeResolveIndirectPass();
  xdec::pass::Context context(function);
  context.setImage(image.reader());
  auto result = pass->run(context);
  const std::string error = result ? std::string{} : result.error().format();
  INFO(error);
  REQUIRE(result);
  CHECK(*result);

  const auto targets = function.targets(terminatorOf(function, entry));
  REQUIRE(targets.size() == 2);
  CHECK(std::find(targets.begin(), targets.end(), handler) != targets.end());
  CHECK(std::find(targets.begin(), targets.end(), entry) != targets.end());
}

TEST_CASE("a branch through a bare leaked entry register resolves once EntryRegFacts anchors it",
          "[passes][resolve-indirect]") {
  // `br x2` with nothing else in the function ever defining x2 -- the
  // obfuscated-entry shape this whole facility exists for (see
  // analysis/entry_reg.h and docs/20-absd-entry-registers.md, where the real
  // register is x22; x2 stands in here because the fixture's register file
  // only models x0..x7). With no facts at all this is exactly
  // `unresolvable()` above, and only EntryRegFacts binding the register
  // changes that.
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const BlockId target = function.createBlock(0x2000);
  function.appendIndirectBranch(entry, 0x1004,
                                function.entryReg(function.registers().find("x2")));
  function.rebuildEdges();
  function.setMaturity(Maturity::Ssa);

  FakeTableImage image;
  image.qwords[0x2000] = 0;  // readable(), so the resolved address counts as code

  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromLiteral(0x2000));

  CHECK_FALSE(run(function, /*seal=*/false, image.reader(), nullptr));  // no facts: unresolved
  CHECK(run(function, /*seal=*/false, image.reader(), &facts));
  const auto targets = function.targets(terminatorOf(function, entry));
  REQUIRE(targets.size() == 1);
  CHECK(targets[0] == target);
}
