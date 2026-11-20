// L0 synthetic tests for M2: architecture-independent mid-pipeline entry
// (`FunctionBuilder` + `AbiSpec` + `LayeredByteReader` + `decompileFromIL`).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include "xdec/analysis/abi.h"
#include "xdec/api.h"
#include "xdec/devirt/memory.h"
#include "xdec/il/builder.h"
#include "xdec/passes/builtin.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::AbiSpec;
using xdec::decompile::DecompileFromILOptions;
using xdec::decompile::DecompileToCResult;
using xdec::decompile::decompileFromIL;
using xdec::devirt::LayeredByteReader;
using xdec::devirt::MemoryRegion;
using xdec::devirt::MemoryRegionMap;
using xdec::devirt::RegionKind;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::FunctionBuilder;
using xdec::il::Maturity;
using xdec::il::RegClass;
using xdec::il::RegId;
using xdec::il::RegisterFile;

TEST_CASE("LayeredByteReader overlays override base bytes and expose immutability",
          "[devirt][memory]") {
  std::array<uint8_t, 16> baseBytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  xdec::ByteReader base = [&](uint64_t va, std::span<std::byte> out) -> xdec::Result<void> {
    if (va >= 0x1000 && va + out.size() <= 0x1000 + baseBytes.size()) {
      std::memcpy(out.data(), baseBytes.data() + (va - 0x1000), out.size());
      return {};
    }
    return xdec::err(xdec::DiagCode::UnmappedAddress, "unmapped");
  };
  xdec::MemoryFacts baseFacts;
  baseFacts.immutable = [](uint64_t va, uint64_t sz) {
    return va >= 0x1000 && va + sz <= 0x1010;
  };

  LayeredByteReader layered(base, baseFacts);
  layered.addOverlay(0x1004, {0xaa, 0xbb, 0xcc, 0xdd}, /*immutable=*/false);
  layered.addOverlay(0x20000, {0x11, 0x22, 0x33, 0x44}, /*immutable=*/true);

  std::array<std::byte, 4> buf{};
  REQUIRE(layered.read(0x1000, buf).hasValue());
  CHECK(static_cast<uint8_t>(buf[0]) == 0);
  CHECK(static_cast<uint8_t>(buf[3]) == 3);

  REQUIRE(layered.read(0x1004, buf).hasValue());
  CHECK(static_cast<uint8_t>(buf[0]) == 0xaa);
  CHECK(static_cast<uint8_t>(buf[3]) == 0xdd);
  CHECK_FALSE(layered.isImmutable(0x1004, 4));
  CHECK(layered.isImmutable(0x1000, 4));

  REQUIRE(layered.read(0x20000, buf).hasValue());
  CHECK(static_cast<uint8_t>(buf[0]) == 0x11);
  CHECK(layered.isImmutable(0x20000, 4));

  const xdec::MemoryFacts facts = layered.asMemoryFacts();
  CHECK(facts.isImmutable(0x20000, 4));
  CHECK_FALSE(facts.isImmutable(0x1004, 4));
}

TEST_CASE("decompileFromIL lowers a 16-register Custom64 virtual ISA loop with stack and rodata",
          "[decompile][from-il][abi]") {
  // 16-register custom virtual ISA: v0..v15, v13 = stack pointer, v15 = link register.
  RegisterFile rf;
  std::vector<RegId> v(16);
  for (unsigned i = 0; i < 16; ++i) {
    const RegClass cls = (i == 13) ? RegClass::StackPointer : RegClass::General;
    v[i] = rf.add(std::format("v{}", i), 64, cls);
  }

  AbiSpec abi;
  abi.name = "vm16-64";
  abi.stackPointer = v[13];
  abi.linkRegister = v[15];
  abi.intArgRegs = {v[0], v[1], v[2], v[3]};
  abi.intRetRegs = {v[0]};
  abi.calleeSavedRegs = {v[8], v[9], v[10], v[11], v[12], v[14], v[15]};
  abi.callerSavedRegs = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};

  // Immutable `.rodata` overlay holding multiplier = 133 (0x85) at 0x10000290.
  LayeredByteReader mem;
  mem.addOverlay(0x10000290, {0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                 /*immutable=*/true);

  MemoryRegionMap regions;
  regions.add(MemoryRegion{"vm_rodata", RegionKind::ImmutableData, 0x10000290, std::nullopt, 8});
  regions.add(MemoryRegion{"vm_stack", RegionKind::PrivateStack, std::nullopt, v[13], 0x1000});

  FunctionBuilder b(Arch::Custom64, rf, 0x10004000, "roll_hash");
  const BlockId entry = b.createBlock(0x10004000);
  const BlockId header = b.createBlock(0x10004018);
  const BlockId loop = b.createBlock(0x10004020);
  const BlockId exitBlk = b.createBlock(0x10004060);

  // entry: allocate 16B stack frame on v13, store initial acc=0 to [v13+0x8],
  //        load immutable multiplier from 0x10000290 into v4, v2 = 0 (index)
  b.setInsertPoint(entry);
  b.setCurrentVa(0x10004000);
  const ExprId frameSp = b.sub(b.readReg(v[13]), b.i64(16));
  b.writeReg(v[13], frameSp);
  b.store(64, b.add(b.readReg(v[13]), b.i64(8)), b.i64(0));
  b.writeReg(v[4], b.load(64, b.i64(0x10000290)));
  b.writeReg(v[2], b.i64(0));
  b.branch(header);

  // header: while (v2 < v1)
  b.setInsertPoint(header);
  b.setCurrentVa(0x10004018);
  b.condBranch(b.cmp(ExprOp::CmpLtU, b.readReg(v[2]), b.readReg(v[1])), loop, exitBlk);

  // loop: acc = (acc * mult + *(uint8_t*)(v0 + v2)) & 0xff; v2++; goto header
  b.setInsertPoint(loop);
  b.setCurrentVa(0x10004020);
  const ExprId acc = b.load(64, b.add(b.readReg(v[13]), b.i64(8)));
  const ExprId ch = b.zext(64, b.load(8, b.add(b.readReg(v[0]), b.readReg(v[2]))));
  const ExprId nextAcc = b.and_(b.add(b.mul(acc, b.readReg(v[4])), ch), b.i64(0xff));
  b.store(64, b.add(b.readReg(v[13]), b.i64(8)), nextAcc);
  const ExprId nextIdx = b.add(b.readReg(v[2]), b.i64(1));
  b.writeReg(v[2], nextIdx);
  b.branch(header);

  // exit: v0 = *(v13 + 8); restore v13; ret
  b.setInsertPoint(exitBlk);
  b.setCurrentVa(0x10004060);
  b.writeReg(v[0], b.load(64, b.add(b.readReg(v[13]), b.i64(8))));
  b.writeReg(v[13], b.add(b.readReg(v[13]), b.i64(16)));
  b.ret();

  auto built = b.finalize(Maturity::Cfg);
  const std::string buildErr = built ? std::string{} : built.error().format();
  INFO(buildErr);
  REQUIRE(built.hasValue());

  xdec::pass::Registry registry;
  xdec::passes::registerBuiltinPasses(registry);

  DecompileFromILOptions opts;
  opts.abi = abi;
  opts.layeredMemory = &mem;
  opts.regions = &regions;
  opts.emit.structuredExprs = true;
  opts.captureAst = true;

  auto res = decompileFromIL(std::move(*built), registry, opts);
  const std::string decErr = res ? std::string{} : res.error().format();
  INFO(decErr);
  REQUIRE(res.hasValue());

  const std::string& c = res->cSource;
  INFO(c);
  CHECK(c.find("roll_hash(") != std::string::npos);
  CHECK(c.find("arg1") != std::string::npos);
  CHECK(c.find("arg2") != std::string::npos);
  // Immutable `.rodata` load from 0x10000290 must be folded to 0x85!
  CHECK(c.find("0x85") != std::string::npos);
  CHECK(c.find("10000290") == std::string::npos);
  // Loop structured cleanly with zero gotos.
  CHECK(res->report.labeledBlockCount == 0);
  CHECK((c.find("while (") != std::string::npos || c.find("do {") != std::string::npos));
  REQUIRE(res->ast.has_value());
  CHECK(res->ast->body != nullptr);
}
