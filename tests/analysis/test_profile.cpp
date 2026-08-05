// Obfuscation profiling: the signals and the thresholds over them.
#include <catch2/catch_test_macros.hpp>

#include "il/il_test_support.h"
#include "xdec/analysis/profile.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId block(uint64_t va) {
    const BlockId id = function.createBlock(va);
    if (!function.entryBlock().valid()) {
      function.setEntryBlock(id);
    }
    return id;
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }

  Function function;
};

TEST_CASE("an honest function profiles clean", "[analysis][profile]") {
  Fixture f;
  const BlockId entry = f.block(0x1000);
  f.function.appendStore(entry, 0x1000, Type::integer(64), f.i64(0x9000), f.i64(1));
  f.function.appendReturn(entry, 0x1004);
  f.function.rebuildEdges();

  const xdec::analysis::ObfuscationProfile p = xdec::analysis::profile(f.function);
  CHECK(p.blocks == 1);
  CHECK(p.indirectBranches == 0);
  CHECK(p.unresolvedIndirect == 0);
  CHECK(p.mbaExpressions == 0);
  CHECK(!p.likelyFlattened());
  CHECK(!p.likelyMba());
}

TEST_CASE("a dispatcher with heavy fan-in reads as flattened", "[analysis][profile]") {
  Fixture f;
  // The flattening signature before resolution: a dispatcher block whose
  // indirect branch is unresolved, fed by every state block looping back.
  const BlockId dispatcher = f.block(0x1000);
  f.function.appendIndirectBranch(dispatcher, 0x1000, f.i64(0));
  for (unsigned i = 0; i < 9; ++i) {
    const BlockId state = f.block(0x1010 + i * 0x10);
    f.function.appendBranch(state, 0x1010 + i * 0x10, dispatcher);
  }
  f.function.rebuildEdges();

  const xdec::analysis::ObfuscationProfile p = xdec::analysis::profile(f.function);
  CHECK(p.dispatcherFanIn == 9);
  CHECK(p.unresolvedIndirect == 1);
  CHECK(p.likelyFlattened());
}

TEST_CASE("mixed arithmetic-bitwise trees count as MBA", "[analysis][profile]") {
  Fixture f;
  const BlockId entry = f.block(0x1000);
  const ExprId x = f.function.entryReg(f.function.registers().find("x0"));
  const ExprId y = f.function.entryReg(f.function.registers().find("x1"));
  const Type t = Type::integer(64);
  // ((x^y) + 2·(x&y)) — the MBA signature, stored so the pool keeps it.
  const ExprId mba = f.function.binary(
      ExprOp::Add, f.function.binary(ExprOp::Xor, x, y),
      f.function.binary(ExprOp::Mul, f.function.binary(ExprOp::And, x, y),
                        f.function.constant(t, 2)));
  f.function.appendStore(entry, 0x1000, t, f.i64(0x9000), mba);
  f.function.appendReturn(entry, 0x1004);
  f.function.rebuildEdges();

  const xdec::analysis::ObfuscationProfile p = xdec::analysis::profile(f.function);
  CHECK(p.mbaExpressions >= 2);  // the sum and at least one nested mix
}

}  // namespace
