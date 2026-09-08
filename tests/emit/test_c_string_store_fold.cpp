// Emit-level regression for string-store-fold (see
// analysis/string_store_fold.h): a run of constant Stores that decodes to a
// printable, NUL-terminated string prints as one synthesized `strcpy` line,
// with the include it needs, instead of one assignment per Store.
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
  ExprId slot(int64_t delta) {
    const ExprId sp = function.entryReg(function.registers().find("sp"));
    return delta < 0
               ? function.binary(ExprOp::Sub, sp, i64(static_cast<uint64_t>(-delta)))
               : function.binary(ExprOp::Add, sp, i64(static_cast<uint64_t>(delta)));
  }

  std::string emit() {
    function.rebuildEdges();
    const StackFrame frame = StackFrame::compute(function);
    const VariableTable variables = VariableTable::recover(function, frame);
    const Dominators dominators = Dominators::compute(function);
    const PostDominators postDominators = PostDominators::compute(function);
    const std::vector<NaturalLoop> loops = naturalLoops(function, dominators);
    return printFunction(function, variables, frame,
                         structureFunction(function, dominators, postDominators, loops));
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a run of constant stores spelling \"hi\" prints as one synthesized strcpy",
          "[emit][string-store-fold]") {
  Fixture f;
  // "hi\0", a 2-byte store then a 1-byte store, back to back -- the shape
  // sub_192464d44's "com.apple.absd" (docs/22) compiles down to, in
  // miniature. The buffer's address is then handed to a call, exactly like
  // the real function's own `sub_1db481864(..., &var_6b, ...)`: without an
  // escape or a read, findDeadStackStores (shape H1) would delete this
  // otherwise-unread run outright, before this fold ever sees it.
  const ExprId address = f.slot(-0x10);
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), address, f.i64(0x6968));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.slot(-0xe), f.i64(0x0));
  const il::OpId call = f.function.appendCall(f.entry, 0x1008, f.i64(0x9000));
  f.function.setOperands(call, std::vector<ExprId>{f.i64(0x9000), address});
  f.function.appendReturn(f.entry, 0x100c);

  const std::string text = f.emit();
  INFO(text);
  CHECK(text.find("strcpy(&var_10, \"hi\");") != std::string::npos);
  // Neither Store prints its own assignment anymore.
  CHECK(text.find("var_10 = ") == std::string::npos);
  CHECK(text.find("var_e = ") == std::string::npos);
  CHECK(text.find("#include <string.h>") != std::string::npos);
}

TEST_CASE("a decompile that never uses the idiom gets no <string.h> include",
          "[emit][string-store-fold]") {
  Fixture f;
  f.function.appendStore(f.entry, 0x1000, Type::integer(32), f.slot(-0x10),
                         f.function.entryReg(f.function.registers().find("x0")));
  f.function.appendReturn(f.entry, 0x1004);

  const std::string text = f.emit();
  INFO(text);
  CHECK(text.find("#include <string.h>") == std::string::npos);
}

TEST_CASE("a run to a global address keeps its ordinary per-store printing",
          "[emit][string-store-fold]") {
  Fixture f;
  // findFoldableStringStores only ever proposes a StackSlot run (see the
  // header), so a Global address never becomes a candidate in the first
  // place -- this is the same "left exactly as it always was" outcome
  // CContext::addressOfLocal's own filter gives a StackSlot run with no
  // local recovered, just reached a different way.
  f.function.appendStore(f.entry, 0x1000, Type::integer(16), f.i64(0x30c420), f.i64(0x6968));
  f.function.appendStore(f.entry, 0x1004, Type::integer(8), f.i64(0x30c422), f.i64(0x0));
  f.function.appendReturn(f.entry, 0x1008);

  const std::string text = f.emit();
  INFO(text);
  CHECK(text.find("strcpy(") == std::string::npos);
  CHECK(text.find("#include <string.h>") == std::string::npos);
}
