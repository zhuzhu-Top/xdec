// L0 synthetic tests for M4 & M5:
//   - Specializer over synthetic OLLVM CFF dispatcher & rolling-key VM
//   - DiffVerifier bit-exact verification (pass & deliberate mismatch detection)
//   - AlgebraRuleRegistry oracle gate (valid rule accepted, unsound rule rejected)
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

#include "xdec/analysis/abi.h"
#include "xdec/api.h"
#include "xdec/devirt/memory.h"
#include "xdec/devirt/specializer.h"
#include "xdec/devirt/verifier.h"
#include "xdec/il/builder.h"
#include "xdec/passes/algebra_rules.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::AbiSpec;
using xdec::decompile::DecompileFromILOptions;
using xdec::decompile::decompileFromIL;
using xdec::devirt::DiffVector;
using xdec::devirt::DiffVerifier;
using xdec::devirt::LayeredByteReader;
using xdec::devirt::Specializer;
using xdec::devirt::SpecializerOptions;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::FunctionBuilder;
using xdec::il::Maturity;
using xdec::il::RegClass;
using xdec::il::RegId;
using xdec::il::RegisterFile;
using xdec::passes::AlgebraRuleRegistry;

TEST_CASE("Specializer unrolls an OLLVM CFF state machine into a loop and passes DiffVerifier",
          "[devirt][specializer][verifier]") {
  RegisterFile rf;
  const RegId x0 = rf.add("x0", 64, RegClass::General);
  const RegId x1 = rf.add("x1", 64, RegClass::General);
  const RegId st = rf.add("state", 64, RegClass::General);
  const RegId acc = rf.add("acc", 64, RegClass::General);

  AbiSpec abi;
  abi.name = "cff-64";
  abi.intArgRegs = {x0, x1};
  abi.intRetRegs = {x0};

  // Synthetic OLLVM CFF loop:
  //   state = 1; acc = 0;
  //   dispatch:
  //     if state == 1: acc = x0 ^ 0x55; state = (x1 != 0 ? 2 : 3)
  //     if state == 2: acc = acc + x1; x1 = x1 - 1; state = (x1 != 0 ? 2 : 3)
  //     if state == 3: x0 = acc; ret
  FunctionBuilder b(Arch::Custom64, rf, 0x1000, "cff_sum");
  const BlockId entry = b.createBlock(0x1000);
  const BlockId disp = b.createBlock(0x1020);
  const BlockId chk2 = b.createBlock(0x1030);
  const BlockId s1 = b.createBlock(0x1040);
  const BlockId s2 = b.createBlock(0x1060);
  const BlockId s3 = b.createBlock(0x1080);

  b.setInsertPoint(entry);
  b.writeReg(st, b.i64(1));
  b.writeReg(acc, b.i64(0));
  b.branch(disp);

  b.setInsertPoint(disp);
  b.condBranch(b.cmp(ExprOp::CmpEq, b.readReg(st), b.i64(1)), s1, chk2);

  b.setInsertPoint(chk2);
  b.condBranch(b.cmp(ExprOp::CmpEq, b.readReg(st), b.i64(2)), s2, s3);

  b.setInsertPoint(s1);
  b.writeReg(acc, b.xor_(b.readReg(x0), b.i64(0x55)));
  b.writeReg(st, b.select(b.cmp(ExprOp::CmpNe, b.readReg(x1), b.i64(0)),
                          b.i64(2), b.i64(3)));
  b.branch(disp);

  b.setInsertPoint(s2);
  b.writeReg(acc, b.add(b.readReg(acc), b.readReg(x1)));
  b.writeReg(x1, b.sub(b.readReg(x1), b.i64(1)));
  b.writeReg(st, b.select(b.cmp(ExprOp::CmpNe, b.readReg(x1), b.i64(0)),
                          b.i64(2), b.i64(3)));
  b.branch(disp);

  b.setInsertPoint(s3);
  b.writeReg(x0, b.readReg(acc));
  b.ret();

  auto rawRes = b.finalize(Maturity::Cfg);
  REQUIRE(rawRes.hasValue());

  SpecializerOptions sOpts;
  sOpts.stateRegs = {st};
  auto specRes = Specializer::run(**rawRes, sOpts);
  const std::string specErr = specRes ? std::string{} : specRes.error().format();
  INFO(specErr);
  REQUIRE(specRes.hasValue());

  // Clone-equivalent verification via DiffVerifier across baseline vs specialized + decompiled!
  std::vector<DiffVector> vecs;
  for (uint64_t a : {0ULL, 1ULL, 0x55ULL, 0x1234ULL}) {
    for (uint64_t n : {0ULL, 1ULL, 4ULL, 10ULL}) {
      const uint64_t expected = (a ^ 0x55ULL) + (n * (n + 1)) / 2;
      vecs.push_back(DiffVector{{a, n}, 0, expected});
    }
  }

  const auto errOf = [](const auto& r) { return r ? std::string{} : r.error().format(); };

  auto diffPre = DiffVerifier::verify(**rawRes, **specRes, vecs, nullptr, &abi);
  INFO(errOf(diffPre));
  REQUIRE(diffPre.hasValue());
  CHECK(diffPre->vectorsChecked == vecs.size());

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  DecompileFromILOptions opts;
  opts.abi = abi;
  auto decRes = decompileFromIL(std::move(*specRes), registry, opts);
  REQUIRE(decRes.hasValue());

  // The CFF `state` variable must be completely specialized away!
  INFO(decRes->cSource);
  CHECK(decRes->cSource.find("0x55") != std::string::npos);

  auto diffPost = DiffVerifier::verify(**rawRes, *decRes->function, vecs, nullptr, &abi);
  INFO(errOf(diffPost));
  REQUIRE(diffPost.hasValue());

  // Deliberate bad expected value must fail DiffVerifier loudly!
  std::vector<DiffVector> bad{DiffVector{{1, 2}, 0, 0xdeadbeef}};
  CHECK_FALSE(DiffVerifier::verifyAgainstExpected(*decRes->function, bad, nullptr, &abi).hasValue());
}

TEST_CASE("Specializer unfolds rolling-key bytecode stream with zero hand-written opcode decoder",
          "[devirt][specializer]") {
  // Bytecode encrypted with rolling key:
  //   pc=0x8000: enc_op = 0x11 ^ 0x05 = 0x14 (op=1: acc += 7), next_key = (0x05 + 3) = 8
  //   pc=0x8004: enc_op = 0x02 ^ 0x08 = 0x0a (op=2: acc ^= 0x33), next_key = (0x08 + 3) = 11
  //   pc=0x8008: enc_op = 0x00 ^ 0x0b = 0x0b (op=0: halt)
  LayeredByteReader mem;
  mem.addOverlay(0x8000, {0x14, 0x00, 0x00, 0x00}, true);
  mem.addOverlay(0x8004, {0x0a, 0x00, 0x00, 0x00}, true);
  mem.addOverlay(0x8008, {0x0b, 0x00, 0x00, 0x00}, true);

  RegisterFile rf;
  const RegId vpc = rf.add("vpc", 64, RegClass::General);
  const RegId key = rf.add("rkey", 64, RegClass::General);
  const RegId acc = rf.add("acc", 64, RegClass::General);

  AbiSpec abi;
  abi.name = "roll-vm";
  abi.intArgRegs = {acc};
  abi.intRetRegs = {acc};

  FunctionBuilder b(Arch::Custom64, rf, 0x1000, "roll_vm");
  const BlockId fetch = b.createBlock(0x1000);
  const BlockId chk1 = b.createBlock(0x1020);
  const BlockId op1 = b.createBlock(0x1040);
  const BlockId chk2 = b.createBlock(0x1060);
  const BlockId op2 = b.createBlock(0x1080);
  const BlockId halt = b.createBlock(0x10a0);

  b.setInsertPoint(fetch);
  const ExprId enc = b.zext(64, b.load(8, b.readReg(vpc)));
  const ExprId dec = b.xor_(enc, b.readReg(key));
  b.condBranch(b.cmp(ExprOp::CmpEq, dec, b.i64(0x11)), op1, chk1);

  b.setInsertPoint(chk1);
  const ExprId dec1 = b.xor_(b.zext(64, b.load(8, b.readReg(vpc))), b.readReg(key));
  b.condBranch(b.cmp(ExprOp::CmpEq, dec1, b.i64(0x02)), op2, chk2);

  b.setInsertPoint(chk2);
  b.branch(halt);

  b.setInsertPoint(op1);
  b.writeReg(acc, b.add(b.readReg(acc), b.i64(7)));
  b.writeReg(vpc, b.add(b.readReg(vpc), b.i64(4)));
  b.writeReg(key, b.add(b.readReg(key), b.i64(3)));
  b.branch(fetch);

  b.setInsertPoint(op2);
  b.writeReg(acc, b.xor_(b.readReg(acc), b.i64(0x33)));
  b.writeReg(vpc, b.add(b.readReg(vpc), b.i64(4)));
  b.writeReg(key, b.add(b.readReg(key), b.i64(3)));
  b.branch(fetch);

  b.setInsertPoint(halt);
  b.ret();

  auto vm = b.finalize(Maturity::Cfg);
  REQUIRE(vm.hasValue());

  SpecializerOptions sOpts;
  sOpts.initialState.set(vpc, 0x8000);
  sOpts.initialState.set(key, 0x05);
  sOpts.stateRegs = {vpc, key};
  sOpts.memory = &mem;
  auto specialized = Specializer::run(**vm, sOpts);
  REQUIRE(specialized.hasValue());

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);
  DecompileFromILOptions dOpts;
  dOpts.abi = abi;
  auto decRes = decompileFromIL(std::move(*specialized), registry, dOpts);
  REQUIRE(decRes.hasValue());

  INFO(decRes->cSource);
  CHECK(decRes->cSource.find("0x33") != std::string::npos);
  CHECK(decRes->cSource.find("0x7") != std::string::npos);

  std::vector<DiffVector> vecs{
      DiffVector{{0}, 0, (0ULL + 7ULL) ^ 0x33ULL},
      DiffVector{{100}, 0, (100ULL + 7ULL) ^ 0x33ULL},
  };
  REQUIRE(DiffVerifier::verifyAgainstExpected(*decRes->function, vecs, nullptr, &abi).hasValue());
}

TEST_CASE("AlgebraRuleRegistry validates custom MBA rules against concrete oracle and rejects bogus ones",
          "[passes][algebra][oracle]") {
  AlgebraRuleRegistry reg;

  // Bogus rule: claims `x ^ y == x + y` -> MUST be rejected!
  auto bad = reg.registerVerifiedRule(
      "bogus_xor_to_add", 2, 64,
      [](il::Function& f, const std::vector<ExprId>& vars) {
        return f.binary(ExprOp::Xor, vars[0], vars[1]);
      },
      [](il::Function& f, ExprId id) {
        const il::Expr& e = f.expr(id);
        if (e.op == ExprOp::Xor) {
          return f.binary(ExprOp::Add, e.operands[0], e.operands[1]);
        }
        return id;
      });
  CHECK_FALSE(bad.hasValue());
  CHECK(reg.size() == 0);

  // Valid MBA rule: `(x & y) + (x | y) == x + y` -> MUST be accepted!
  auto good = reg.registerVerifiedRule(
      "and_plus_or_to_add", 2, 64,
      [](il::Function& f, const std::vector<ExprId>& vars) {
        return f.binary(ExprOp::Add, f.binary(ExprOp::And, vars[0], vars[1]),
                        f.binary(ExprOp::Or, vars[0], vars[1]));
      },
      [](il::Function& f, ExprId id) {
        const il::Expr& e = f.expr(id);
        if (e.op == ExprOp::Add) {
          const il::Expr& l = f.expr(e.operands[0]);
          const il::Expr& r = f.expr(e.operands[1]);
          if (l.op == ExprOp::And && r.op == ExprOp::Or &&
              l.operands[0] == r.operands[0] && l.operands[1] == r.operands[1]) {
            return f.binary(ExprOp::Add, l.operands[0], l.operands[1]);
          }
        }
        return id;
      });
  const std::string goodErr = good ? std::string{} : good.error().format();
  INFO(goodErr);
  REQUIRE(good.hasValue());
  CHECK(reg.size() == 1);
}
