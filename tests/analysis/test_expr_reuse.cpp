// ExpressionReuseReport: exact duplicates across a block/terminator pair and
// structural duplicates across a repeated Load, plus the negative cases that
// must NOT fire (a genuinely fresh value, a Load separated by a clobber).
#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/expr_reuse.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::analyzeExpressionReuse;
using xdec::analysis::ReuseKind;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId x0() { return function.entryReg(function.registers().find("x0")); }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a value stored in a block and reused as its own resolved switch index is an exact duplicate",
          "[analysis][expr-reuse]") {
  Fixture f;
  // flag = x0 + 1: a real computed expression (a bare Value would not be
  // "worth reporting" -- see analyzeExpressionReuse's own threshold).
  const ExprId flag = f.function.binary(ExprOp::Add, f.x0(), f.i64(1));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000), flag);
  // A pointer-table computed branch whose index is exactly `flag`.
  const ExprId address = f.function.binary(
      ExprOp::Add, f.i64(0x30b7f0), f.function.binary(ExprOp::Shl, flag, f.i64(3)));
  const ExprId target = f.function.valueRef(
      f.function.appendLoad(f.entry, 0x1004, Type::integer(64), address));
  f.function.appendIndirectBranch(f.entry, 0x1008, target);

  const auto report = analyzeExpressionReuse(f.function);
  const auto exact = report.count(ReuseKind::ExactDuplicate);
  CHECK(exact == 1);
  REQUIRE(report.findings.size() == 1);
  CHECK(report.findings.front().shared == flag);
}

TEST_CASE("a value stored and then branched on directly is an exact duplicate",
          "[analysis][expr-reuse]") {
  Fixture f;
  const ExprId flag = f.function.binary(ExprOp::Add, f.x0(), f.i64(2));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000), flag);
  const BlockId taken = f.function.createBlock(0x2000);
  const BlockId other = f.function.createBlock(0x3000);
  f.function.appendCondBranch(f.entry, 0x1004, flag, taken, other);
  f.function.appendReturn(taken, 0x2000);
  f.function.appendReturn(other, 0x3000);

  const auto report = analyzeExpressionReuse(f.function);
  CHECK(report.count(ReuseKind::ExactDuplicate) == 1);
}

TEST_CASE("a value used only once is not reported", "[analysis][expr-reuse]") {
  Fixture f;
  const ExprId flag = f.function.binary(ExprOp::Add, f.x0(), f.i64(3));
  f.function.appendStore(f.entry, 0x1000, Type::integer(64), f.i64(0x9000), flag);
  f.function.appendReturn(f.entry, 0x1004);

  const auto report = analyzeExpressionReuse(f.function);
  CHECK(report.findings.empty());
}

TEST_CASE("two loads of the same address with nothing between are a structural duplicate",
          "[analysis][expr-reuse]") {
  Fixture f;
  const ExprId address = f.function.binary(ExprOp::Add, f.x0(), f.i64(0x10));
  f.function.appendLoad(f.entry, 0x1000, Type::integer(64), address);
  f.function.appendLoad(f.entry, 0x1004, Type::integer(64), address);
  f.function.appendReturn(f.entry, 0x1008);

  const auto report = analyzeExpressionReuse(f.function);
  REQUIRE(report.count(ReuseKind::StructuralDuplicate) == 1);
  const auto& finding =
      *std::find_if(report.findings.begin(), report.findings.end(), [](const auto& item) {
        return item.kind == ReuseKind::StructuralDuplicate;
      });
  CHECK(finding.shared == address);
}

TEST_CASE("a store between two loads of the same address clears the candidate",
          "[analysis][expr-reuse]") {
  Fixture f;
  const ExprId address = f.function.binary(ExprOp::Add, f.x0(), f.i64(0x10));
  f.function.appendLoad(f.entry, 0x1000, Type::integer(64), address);
  f.function.appendStore(f.entry, 0x1004, Type::integer(64), f.i64(0x9000), f.i64(0));
  f.function.appendLoad(f.entry, 0x1008, Type::integer(64), address);
  f.function.appendReturn(f.entry, 0x100c);

  const auto report = analyzeExpressionReuse(f.function);
  CHECK(report.count(ReuseKind::StructuralDuplicate) == 0);
}
