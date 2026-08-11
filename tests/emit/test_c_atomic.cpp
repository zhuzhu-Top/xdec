// How AArch64's exclusive-access and compare-and-swap instructions print,
// once c_stmt.cpp's peepholes (see analysis/atomic_exclusive.h) have found
// them.
//
// Each instruction the specs split into more than one IL op --
// reserve+load for ldaxr, store+status for stlxr -- is built here exactly as
// loadstore.xspec lifts it, the same way test_c_syscall.cpp builds an svc
// intrinsic by hand: what is under test is the emitter's half of the
// contract, not the lift.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/stack_frame.h"
#include "xdec/analysis/variables.h"
#include "xdec/emit/c_printer.h"
#include "xdec/emit/structure.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::StackFrame;
using xdec::analysis::VariableTable;
using xdec::emit::printFunction;
using xdec::emit::structureFunction;
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
  ExprId entryReg(std::string_view name) {
    return function.entryReg(function.registers().find(name));
  }

  /// Stores `value` to a fixed global, the shortest way to give an
  /// expression a second reader so its defining op is not itself folded into
  /// the first one (see analysis::findFoldableMemoryLoads): what is under
  /// test here is how the op prints on its own line, not whether it gets one.
  void observe(ExprId value, uint32_t width = 64) {
    function.appendStore(entry, 0x1000, Type::integer(width), i64(0x400000), value);
  }

  std::string emit() {
    function.appendReturn(entry, 0x1010);
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators,
                                           loops));
  }

  Function function;
  BlockId entry;
};

[[nodiscard]] bool contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

// The pair loadstore.xspec's ldaxr rule lifts to -- intrinsic("aarch64.reserve",
// addr) immediately before a plain Load of the same address -- is exactly the
// shape analysis::findExclusiveLoads looks for.
TEST_CASE("an exclusive load fuses its reservation into __ldaxr",
          "[emit][atomic]") {
  Fixture f;
  const ExprId address = f.entryReg("x0");
  f.function.appendIntrinsic(f.entry, 0x1000, "aarch64.reserve", Type::voidType(),
                             std::vector<ExprId>{address});
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(32), address);
  const ExprId loadedExpr = f.function.valueRef(loaded);
  // Two readers, not one, so the load keeps its own statement instead of
  // being inlined into whichever reader came first.
  f.observe(loadedExpr, 32);
  f.observe(loadedExpr, 32);

  const std::string text = f.emit();
  INFO(text);
  // apply_types infers arg1 as uint32_t* from this very access, so
  // pointerOperand adds no redundant cast on top of it.
  CHECK(contains(text, "__ldaxr32(arg1)"));
  CHECK(!contains(text, "reserve"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.ldaxr"));
}

// stlxr's split is the same shape the other way round: a plain Store,
// immediately followed by intrinsic_value("aarch64.store_exclusive_status").
TEST_CASE("an exclusive store fuses its status read into __stlxr",
          "[emit][atomic]") {
  Fixture f;
  const ExprId address = f.entryReg("x0");
  const ExprId value = f.entryReg("x1");
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), address, value);
  const il::OpId status = f.function.appendIntrinsic(
      f.entry, 0x1004, "aarch64.store_exclusive_status", Type::integer(32), {});
  f.observe(f.function.valueRef(f.function.op(status).result), 32);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "__stlxr32(arg2, arg1)"));
  CHECK(!contains(text, "store_exclusive_status"));
}

// casal has no unconditional-load-then-store IL shape -- the store only
// happens when the compare succeeds -- so specs/arm64/loadstore.xspec's
// cas/casa/casl/casal rules lift it as one intrinsic_value carrying the
// address, the expected value and the new value in and the old value out.
// printCas expands that back into the logical (non-atomic) C sequence a
// reader of the disassembly would write by hand.
TEST_CASE("a compare-and-swap prints as a logical load/compare/store",
          "[emit][atomic]") {
  Fixture f;
  const ExprId address = f.entryReg("x0");
  const ExprId expected = f.entryReg("x1");
  const ExprId desired = f.entryReg("x2");
  const il::OpId cas = f.function.appendIntrinsic(
      f.entry, 0x1000, "aarch64.cas", Type::integer(32),
      std::vector<ExprId>{address, expected, desired});
  f.observe(f.function.valueRef(f.function.op(cas).result), 32);

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "logical compare-and-swap"));
  CHECK(contains(text, "= *(uint32_t*)(arg1);"));
  CHECK(contains(text, "if (arg2 =="));
  CHECK(contains(text, "*(uint32_t*)(arg1) = arg3;"));
  CHECK(!contains(text, "aarch64.cas"));
}

// The full sub_2f93d0-shaped retry loop: __ldaxr32/__stlxr32 fused (see the
// two tests above) inside the guarded do-while their compare-fail exit and
// their status retry both leave through, one and the same block. Structuring
// alone (see test_structure.cpp's "guarded do-while" case) proves the shape;
// this proves the fused intrinsics survive the same rewrite end to end.
TEST_CASE("an LL/SC retry loop's compare-fail exit prints as break, not goto",
          "[emit][atomic]") {
  Fixture f;
  const BlockId body = f.function.createBlock(0x2000);
  const BlockId exit = f.function.createBlock(0x3000);
  const ExprId address = f.entryReg("x2");
  const ExprId expected = f.entryReg("x0");
  const ExprId desired = f.entryReg("x1");

  // Header: __ldaxr32, then exit early on a compare mismatch.
  f.function.appendIntrinsic(f.entry, 0x1000, "aarch64.reserve", Type::voidType(),
                             std::vector<ExprId>{address});
  const il::ValueId loaded = f.function.appendLoad(f.entry, 0x1004, Type::integer(32), address);
  const ExprId loadedExpr = f.function.valueRef(loaded);
  const ExprId mismatch = f.function.binary(ExprOp::CmpNe, loadedExpr, expected);
  f.function.appendCondBranch(f.entry, 0x1008, mismatch, exit, body);

  // Latch: __stlxr32, retrying the header on a nonzero status.
  f.function.appendStore(body, 0x2000, Type::integer(32), address, desired);
  const il::OpId status = f.function.appendIntrinsic(
      body, 0x2004, "aarch64.store_exclusive_status", Type::integer(32), {});
  const ExprId retry =
      f.function.binary(ExprOp::CmpNe, f.function.valueRef(f.function.op(status).result), f.i64(0));
  f.function.appendCondBranch(body, 0x2008, retry, f.entry, exit);

  const il::OpId ret = f.function.appendReturn(exit, 0x3000);
  f.function.setOperands(ret, std::vector<ExprId>{loadedExpr});
  f.function.rebuildEdges();

  const StackFrame frame = StackFrame::compute(f.function);
  const VariableTable variables = VariableTable::recover(f.function, frame);
  const Dominators dominators = Dominators::compute(f.function);
  const PostDominators postDominators = PostDominators::compute(f.function);
  const std::vector<NaturalLoop> loops = naturalLoops(f.function, dominators);
  const std::string text =
      printFunction(f.function, variables, frame,
                    structureFunction(f.function, dominators, postDominators, loops));
  INFO(text);
  CHECK(contains(text, "__ldaxr32("));
  CHECK(contains(text, "__stlxr32("));
  CHECK(contains(text, "break;"));
  CHECK(!contains(text, "goto"));
}

// BTI and PAC/AUT carry no data flow a decompilation should model; printed as
// the call-shaped __xdec_intrin_* fallback they would be indistinguishable
// from an unmodelled instruction that actually matters, so they collapse to a
// comment instead.
TEST_CASE("BTI and PAC/AUT intrinsics print as comments, not calls",
          "[emit][atomic]") {
  Fixture f;
  f.function.appendIntrinsic(f.entry, 0x1000, "aarch64.bti", Type::voidType(),
                             std::vector<ExprId>{f.function.constant(Type::integer(32), 1)});
  f.function.appendIntrinsic(f.entry, 0x1004, "aarch64.pac.ia", Type::voidType(), {});
  f.function.appendIntrinsic(f.entry, 0x1008, "aarch64.aut.ib", Type::voidType(), {});

  const std::string text = f.emit();
  INFO(text);
  CHECK(contains(text, "/* BTI c */"));
  CHECK(contains(text, "/* PAC: sign with key A */"));
  CHECK(contains(text, "/* PAC: authenticate with key B */"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.bti"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.pac"));
  CHECK(!contains(text, "__xdec_intrin_aarch64.aut"));
}
