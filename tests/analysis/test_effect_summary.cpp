// L0 synthetic tests for Dual-View AST (structuredExprs + IL provenance) and
// generic CaseEffectSummary handler classification.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <unordered_map>

#include "il/il_test_support.h"
#include "xdec/analysis/effect_summary.h"
#include "xdec/decompile/emit.h"
#include "xdec/emit/ast.h"
#include "xdec/pass/registry.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::CaseEffectSummary;
using xdec::analysis::summarizeCaseEffects;
using xdec::decompile::DecompileToCOptions;
using xdec::decompile::RenderToCResult;
using xdec::decompile::renderToC;
using xdec::emit::HlExpr;
using xdec::emit::HlStmt;
using xdec::emit::HlStmtKind;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::il::Type;

namespace {

const HlStmt* findSwitch(const HlStmt& stmt) {
  if (stmt.kind == HlStmtKind::Switch) {
    return &stmt;
  }
  for (const auto& item : stmt.items) {
    if (item) {
      if (const HlStmt* found = findSwitch(*item)) {
        return found;
      }
    }
  }
  if (stmt.thenArm) {
    if (const HlStmt* found = findSwitch(*stmt.thenArm)) {
      return found;
    }
  }
  if (stmt.elseArm) {
    if (const HlStmt* found = findSwitch(*stmt.elseArm)) {
      return found;
    }
  }
  return nullptr;
}

bool containsStructuredBinary(const HlExpr* expr) {
  if (expr == nullptr) {
    return false;
  }
  if (expr->kind == xdec::emit::HlExprKind::Binary && expr->ilExpr.valid()) {
    return true;
  }
  return containsStructuredBinary(expr->cond.get()) ||
         containsStructuredBinary(expr->lhs.get()) ||
         containsStructuredBinary(expr->rhs.get());
}

}  // namespace

TEST_CASE("Dual-View structured AST + summarizeCaseEffects classifies a 10-case VM switch",
          "[analysis][effect-summary][ast]") {
  const il::RegisterFile regs = xdec::test::arm64Registers();
  Function fn(Arch::AArch64, regs, 0x1000);

  const BlockId entry = fn.createBlock(0x1000);
  fn.setEntryBlock(entry);
  const BlockId defaultBlk = fn.createBlock(0x1f00);

  std::array<BlockId, 10> cases{};
  for (std::size_t i = 0; i < cases.size(); ++i) {
    cases[i] = fn.createBlock(0x2000 + i * 0x100);
  }

  // Entry: check opcode = (x1 >> 26) & 0x3f < 10
  const ExprId entryInsn = fn.valueRef(fn.appendReadReg(entry, 0x1000, regs.find("x1")));
  const ExprId entryOpcode = fn.binary(
      ExprOp::And, fn.binary(ExprOp::ShrU, entryInsn, fn.constant(Type::integer(64), 26)),
      fn.constant(Type::integer(64), 0x3f));

  const auto fieldsIn = [&](BlockId b, uint64_t va) {
    struct Fields {
      ExprId vmCtx;
      ExprId rd;
      ExprId rs1;
      ExprId rs2;
      ExprId imm;
      ExprId opcode;
    };
    const ExprId vmCtx = fn.valueRef(fn.appendReadReg(b, va, regs.find("x0")));
    const ExprId insn64 = fn.valueRef(fn.appendReadReg(b, va, regs.find("x1")));
    const ExprId rd = fn.binary(
        ExprOp::And, fn.binary(ExprOp::ShrU, insn64, fn.constant(Type::integer(64), 19)),
        fn.constant(Type::integer(64), 0x7f));
    const ExprId rs1 = fn.binary(
        ExprOp::And, fn.binary(ExprOp::ShrU, insn64, fn.constant(Type::integer(64), 12)),
        fn.constant(Type::integer(64), 0x7f));
    const ExprId rs2 = fn.binary(
        ExprOp::And, fn.binary(ExprOp::ShrU, insn64, fn.constant(Type::integer(64), 5)),
        fn.constant(Type::integer(64), 0x7f));
    const ExprId imm = fn.binary(ExprOp::And, insn64, fn.constant(Type::integer(64), 0xfff));
    const ExprId opcode = fn.binary(
        ExprOp::And, fn.binary(ExprOp::ShrU, insn64, fn.constant(Type::integer(64), 26)),
        fn.constant(Type::integer(64), 0x3f));
    return Fields{vmCtx, rd, rs1, rs2, imm, opcode};
  };

  const auto slotAddr = [&](ExprId vmCtx, ExprId regIdx) {
    return fn.binary(ExprOp::Add, vmCtx,
                     fn.binary(ExprOp::Shl, regIdx, fn.constant(Type::integer(64), 3)));
  };
  const auto loadSlot = [&](BlockId b, uint64_t va, ExprId vmCtx, ExprId regIdx) {
    return fn.valueRef(fn.appendLoad(b, va, Type::integer(64), slotAddr(vmCtx, regIdx)));
  };
  const auto storeSlot = [&](BlockId b, uint64_t va, ExprId vmCtx, ExprId regIdx, ExprId val) {
    fn.appendStore(b, va, Type::integer(64), slotAddr(vmCtx, regIdx), val);
  };

  // Switch table in `.rodata` @0x9000, 10 entries, default on out-of-range.
  const ExprId inRange =
      fn.binary(ExprOp::CmpLtU, entryOpcode, fn.constant(Type::integer(64), 10));
  const BlockId jtDispatch = fn.createBlock(0x1020);
  fn.appendCondBranch(entry, 0x1010, inRange, jtDispatch, defaultBlk);

  const auto jtFields = fieldsIn(jtDispatch, 0x1020);
  const ExprId tableEntry = fn.valueRef(fn.appendLoad(
      jtDispatch, 0x1020, Type::integer(64),
      fn.binary(ExprOp::Add, fn.constant(Type::integer(64), 0x9000),
                fn.binary(ExprOp::Shl, jtFields.opcode, fn.constant(Type::integer(64), 3)))));
  const il::OpId indBr = fn.appendIndirectBranch(jtDispatch, 0x1024, tableEntry);
  std::vector<BlockId> targets(cases.begin(), cases.end());
  fn.setTargets(indBr, targets);

  // Case 0..3: 64-bit ALU (Xor, Add, Sub, RotR)
  const std::array<ExprOp, 4> aluOps{ExprOp::Xor, ExprOp::Add, ExprOp::Sub, ExprOp::RotR};
  for (std::size_t i = 0; i < 4; ++i) {
    const uint64_t va = 0x2000 + i * 0x100;
    const auto f = fieldsIn(cases[i], va);
    const ExprId a = loadSlot(cases[i], va, f.vmCtx, f.rs1);
    const ExprId b = loadSlot(cases[i], va + 4, f.vmCtx, f.rs2);
    storeSlot(cases[i], va + 8, f.vmCtx, f.rd, fn.binary(aluOps[i], a, b));
    fn.appendReturn(cases[i], va + 12);
  }

  // Case 4: 32-bit Add immediate, zero-extended back to 64-bit slot
  {
    const auto f = fieldsIn(cases[4], 0x2400);
    const ExprId a32 =
        fn.cast(ExprOp::Trunc, Type::integer(32), loadSlot(cases[4], 0x2400, f.vmCtx, f.rs1));
    const ExprId i32 = fn.cast(ExprOp::Trunc, Type::integer(32), f.imm);
    const ExprId sum32 = fn.binary(ExprOp::Add, a32, i32);
    storeSlot(cases[4], 0x2408, f.vmCtx, f.rd, fn.cast(ExprOp::ZExt, Type::integer(64), sum32));
    fn.appendReturn(cases[4], 0x240c);
  }

  // Case 5: Load 32-bit unsigned from [R[rs1] + imm] into R[rd]
  {
    const auto f = fieldsIn(cases[5], 0x2500);
    const ExprId base = loadSlot(cases[5], 0x2500, f.vmCtx, f.rs1);
    const ExprId mem = fn.valueRef(fn.appendLoad(
        cases[5], 0x2504, Type::integer(32), fn.binary(ExprOp::Add, base, f.imm)));
    storeSlot(cases[5], 0x2508, f.vmCtx, f.rd, fn.cast(ExprOp::ZExt, Type::integer(64), mem));
    fn.appendReturn(cases[5], 0x250c);
  }

  // Case 6: Load 16-bit signed from [R[rs1] + imm] into R[rd]
  {
    const auto f = fieldsIn(cases[6], 0x2600);
    const ExprId base = loadSlot(cases[6], 0x2600, f.vmCtx, f.rs1);
    const ExprId mem = fn.valueRef(fn.appendLoad(
        cases[6], 0x2604, Type::integer(16), fn.binary(ExprOp::Add, base, f.imm)));
    storeSlot(cases[6], 0x2608, f.vmCtx, f.rd, fn.cast(ExprOp::SExt, Type::integer(64), mem));
    fn.appendReturn(cases[6], 0x260c);
  }

  // Case 7: Store 32-bit R[rd] to [R[rs1] + imm]
  {
    const auto f = fieldsIn(cases[7], 0x2700);
    const ExprId base = loadSlot(cases[7], 0x2700, f.vmCtx, f.rs1);
    const ExprId val =
        fn.cast(ExprOp::Trunc, Type::integer(32), loadSlot(cases[7], 0x2704, f.vmCtx, f.rd));
    fn.appendStore(cases[7], 0x2708, Type::integer(32), fn.binary(ExprOp::Add, base, f.imm), val);
    fn.appendReturn(cases[7], 0x270c);
  }

  // Case 8: Conditional branch updating vPC at vm_ctx + 0x400
  {
    const auto f = fieldsIn(cases[8], 0x2800);
    const BlockId taken = fn.createBlock(0x2840);
    const BlockId fall = fn.createBlock(0x2880);
    const ExprId a = loadSlot(cases[8], 0x2800, f.vmCtx, f.rs1);
    const ExprId b = loadSlot(cases[8], 0x2804, f.vmCtx, f.rs2);
    fn.appendCondBranch(cases[8], 0x2808, fn.binary(ExprOp::CmpEq, a, b), taken, fall);
    const auto ft = fieldsIn(taken, 0x2840);
    fn.appendStore(taken, 0x2840, Type::integer(64),
                   fn.binary(ExprOp::Add, ft.vmCtx, fn.constant(Type::integer(64), 0x400)),
                   ft.imm);
    fn.appendReturn(taken, 0x2844);
    const auto ff = fieldsIn(fall, 0x2880);
    fn.appendStore(fall, 0x2880, Type::integer(64),
                   fn.binary(ExprOp::Add, ff.vmCtx, fn.constant(Type::integer(64), 0x400)),
                   fn.constant(Type::integer(64), 4));
    fn.appendReturn(fall, 0x2884);
  }

  // Case 9: Helper call
  {
    fn.appendCall(cases[9], 0x2900, fn.constant(Type::integer(64), 0x5000));
    fn.appendReturn(cases[9], 0x2904);
  }

  fn.appendReturn(defaultBlk, 0x1f00);
  fn.rebuildEdges();
  fn.setMaturity(Maturity::Cfg);

  // Supply a synthetic jump table in memory so resolve-indirect / Vars pipeline
  // works cleanly through `decompile::renderToC`.
  std::array<uint8_t, 80> jtBytes{};
  for (std::size_t i = 0; i < 10; ++i) {
    const uint64_t t = 0x2000 + i * 0x100;
    for (unsigned b = 0; b < 8; ++b) {
      jtBytes[i * 8 + b] = static_cast<uint8_t>((t >> (8 * b)) & 0xff);
    }
  }
  const xdec::ByteReader mem = [&](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    if (va >= 0x9000 && va + out.size() <= 0x9000 + jtBytes.size()) {
      const std::size_t off = static_cast<std::size_t>(va - 0x9000);
      std::memcpy(out.data(), jtBytes.data() + off, out.size());
      return {};
    }
    return xdec::err(xdec::DiagCode::BadFormat, "unmapped");
  };

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  xdec::pass::Manager manager;
  manager.setImage(mem);
  const auto ran = manager.runTo(fn, registry, Maturity::Vars);
  const std::string errStr = ran ? std::string{} : ran.error().format();
  INFO(errStr);
  REQUIRE(ran.hasValue());

  DecompileToCOptions opts;
  opts.emit.structuredExprs = true;
  opts.captureAst = true;
  const RenderToCResult rendered = renderToC(fn, opts);
  REQUIRE(rendered.ast.has_value());
  REQUIRE(rendered.ast->body != nullptr);

  const HlStmt* sw = findSwitch(*rendered.ast->body);
  REQUIRE(sw != nullptr);
  REQUIRE(sw->cases.size() >= 10);

  std::unordered_map<uint64_t, CaseEffectSummary> byCase;
  bool sawStructuredBin = false;
  for (const auto& c : sw->cases) {
    if (c.isDefault) {
      continue;
    }
    REQUIRE(!c.ilBlocks.empty());
    byCase.emplace(c.value, summarizeCaseEffects(fn, c.ilBlocks));
    if (c.body && !c.body->items.empty()) {
      for (const auto& st : c.body->items) {
        if (st && (containsStructuredBinary(st->lhs.get()) ||
                   containsStructuredBinary(st->rhs.get()))) {
          sawStructuredBin = true;
        }
      }
    }
  }
  CHECK(sawStructuredBin);

  // Verify all 10 cases classified accurately!
  for (std::size_t i = 0; i < 4; ++i) {
    const CaseEffectSummary& s = byCase.at(i);
    REQUIRE(s.stores.size() == 1);
    CHECK(s.stores[0].target.scale == 8);
    CHECK(s.stores[0].value.rootOp == aluOps[i]);
    CHECK(s.stores[0].value.resultBits == 64);
  }
  {
    const CaseEffectSummary& s4 = byCase.at(4);
    REQUIRE(s4.stores.size() == 1);
    CHECK(s4.stores[0].value.zeroExtended);
    CHECK(s4.stores[0].value.rootOp == ExprOp::Add);
    CHECK(fn.expr(s4.stores[0].value.coreExpr).type.bits() == 32);
  }
  {
    const CaseEffectSummary& s5 = byCase.at(5);
    REQUIRE(s5.stores.size() == 1);
    CHECK(s5.stores[0].value.zeroExtended);
    CHECK(s5.stores[0].value.isMemoryLoad);
    CHECK(s5.stores[0].value.loadBits == 32);
    CHECK(s5.stores[0].value.loadSlot.valid());
  }
  {
    const CaseEffectSummary& s6 = byCase.at(6);
    REQUIRE(s6.stores.size() == 1);
    CHECK(s6.stores[0].value.signExtended);
    CHECK(s6.stores[0].value.isMemoryLoad);
    CHECK(s6.stores[0].value.loadBits == 16);
  }
  {
    const CaseEffectSummary& s7 = byCase.at(7);
    REQUIRE(s7.stores.size() == 1);
    CHECK(s7.stores[0].widthBits == 32);
    CHECK(s7.stores[0].target.scale == 1);
  }
  {
    const CaseEffectSummary& s8 = byCase.at(8);
    CHECK(s8.condBranches.size() == 1);
    CHECK(s8.condBranches[0].condOp == ExprOp::CmpEq);
    CHECK(s8.stores.size() == 2);
    CHECK(s8.stores[0].target.offset == 0x400);
    CHECK(s8.stores[1].target.offset == 0x400);
  }
  {
    const CaseEffectSummary& s9 = byCase.at(9);
    REQUIRE(s9.calls.size() == 1);
    CHECK(s9.calls[0].constantTarget == 0x5000);
  }
}
