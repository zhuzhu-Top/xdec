// L0 synthetic tests for RegionMem2Reg, RegionDSE, and multi-block
// Interpreter::runFunction bit-exact equivalence across Cfg and Vars IL.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "xdec/analysis/abi.h"
#include "xdec/api.h"
#include "xdec/devirt/memory.h"
#include "xdec/il/builder.h"
#include "xdec/il/interp.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::AbiSpec;
using xdec::decompile::DecompileFromILOptions;
using xdec::decompile::decompileFromIL;
using xdec::devirt::MemoryRegion;
using xdec::devirt::MemoryRegionMap;
using xdec::devirt::RegionKind;
using xdec::il::BlockId;
using xdec::il::ConcreteValue;
using xdec::il::ExecMemory;
using xdec::il::ExecStop;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::FunctionBuilder;
using xdec::il::Interpreter;
using xdec::il::Maturity;
using xdec::il::OpCode;
using xdec::il::RegClass;
using xdec::il::RegId;
using xdec::il::RegisterFile;

TEST_CASE("RegionMem2Reg promotes private regfile slots and RegionDSE drops dead stores, preserving bit-exact IL execution",
          "[passes][region-mem2reg][interp]") {
  RegisterFile rf;
  const RegId vmCtx = rf.add("vm_ctx", 64, RegClass::General);
  const RegId r0 = rf.add("r0", 64, RegClass::General);
  const RegId sp = rf.add("vsp", 64, RegClass::StackPointer);

  AbiSpec abi;
  abi.name = "vm-ctx-64";
  abi.stackPointer = sp;
  abi.intArgRegs = {vmCtx, r0};
  abi.intRetRegs = {r0};

  MemoryRegionMap regions;
  regions.add(MemoryRegion{"vm_regfile", RegionKind::PrivateRegFile, std::nullopt, vmCtx, 0x100});
  regions.add(MemoryRegion{"vm_stack", RegionKind::PrivateStack, std::nullopt, sp, 0x1000});

  FunctionBuilder b(Arch::Custom64, rf, 0x1000, "vm_diamond");
  const BlockId entry = b.createBlock(0x1000);
  const BlockId left = b.createBlock(0x1020);
  const BlockId right = b.createBlock(0x1040);
  const BlockId join = b.createBlock(0x1060);

  // entry: store dead private stack slot vsp-0x8 = 0xdead;
  //        if (r0 == 0) -> left else -> right
  b.setInsertPoint(entry);
  b.store(64, b.sub(b.readReg(sp), b.i64(8)), b.i64(0xdead));
  b.condBranch(b.cmp(ExprOp::CmpEq, b.readReg(r0), b.i64(0)), left, right);

  // left: vm_ctx+0x08 = 0x111; vm_ctx+0x10 = 0x10
  b.setInsertPoint(left);
  b.store(64, b.add(b.readReg(vmCtx), b.i64(0x08)), b.i64(0x111));
  b.store(64, b.add(b.readReg(vmCtx), b.i64(0x10)), b.i64(0x10));
  b.branch(join);

  // right: vm_ctx+0x08 = 0x222; vm_ctx+0x10 = 0x20
  b.setInsertPoint(right);
  b.store(64, b.add(b.readReg(vmCtx), b.i64(0x08)), b.i64(0x222));
  b.store(64, b.add(b.readReg(vmCtx), b.i64(0x10)), b.i64(0x20));
  b.branch(join);

  // join: r0 = *(vm_ctx+0x08) + *(vm_ctx+0x10); ret
  b.setInsertPoint(join);
  const ExprId a = b.load(64, b.add(b.readReg(vmCtx), b.i64(0x08)));
  const ExprId c = b.load(64, b.add(b.readReg(vmCtx), b.i64(0x10)));
  b.writeReg(r0, b.add(a, c));
  b.ret();

  auto built = b.finalize(Maturity::Cfg);
  REQUIRE(built.hasValue());

  // 1) Execute Cfg-maturity IL via Interpreter::runFunction before optimization.
  const auto runOnce = [&](const il::Function& fn, uint64_t argR0) -> uint64_t {
    ExecMemory mem;
    std::vector<std::byte> blank(0x200, std::byte{0});
    mem.seed(0x500000, blank);
    mem.seed(0x600000, blank);
    Interpreter interp(fn, &mem);
    interp.writeRegister(fn.registers().find("vm_ctx"), ConcreteValue{0x500000, 0});
    interp.writeRegister(fn.registers().find("vsp"), ConcreteValue{0x600100, 0});
    interp.writeRegister(fn.registers().find("r0"), ConcreteValue{argR0, 0});
    const auto out = interp.runFunction(fn.entryBlock());
    REQUIRE(out.stop == ExecStop::Return);
    if (out.returnValue.has_value()) {
      return out.returnValue->lo;
    }
    return interp.readRegister(fn.registers().find("r0")).lo;
  };

  CHECK(runOnce(**built, 0) == 0x121);
  CHECK(runOnce(**built, 1) == 0x242);

  // 2) Run full decompileFromIL with RegionMem2Reg + RegionDSE enabled.
  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  DecompileFromILOptions opts;
  opts.abi = abi;
  opts.regions = &regions;
  auto res = decompileFromIL(std::move(*built), registry, opts);
  const std::string decErr = res ? std::string{} : res.error().format();
  INFO(decErr);
  REQUIRE(res.hasValue());

  // Every Load/Store on vm_ctx+0x08/+0x10/+0x18 must have been eliminated!
  for (const BlockId bid : res->function->blockHandles()) {
    for (const il::OpId oid : res->function->block(bid).ops) {
      const OpCode code = res->function->op(oid).code;
      CHECK(code != OpCode::Load);
      CHECK(code != OpCode::Store);
    }
  }
  INFO(res->cSource);
  CHECK(res->cSource.find("dead") == std::string::npos);

  // 3) Execute optimized Vars-maturity IL via Interpreter::runFunction: bit-exact!
  CHECK(runOnce(*res->function, 0) == 0x121);
  CHECK(runOnce(*res->function, 1) == 0x242);
}
