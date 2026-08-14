// ImageEval: bounded value sets over undef-tolerant evaluation, loads served
// by a fake image.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "il/il_test_support.h"
#include "xdec/analysis/image_eval.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::ByteReader;
using xdec::analysis::ImageEval;
using xdec::analysis::ValueSet;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

/// A byte image assembled from 8-byte qwords, for table loads.
struct FakeImage {
  std::map<uint64_t, uint64_t> qwords;

  [[nodiscard]] ByteReader reader() const {
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
  FakeImage image;
};

TEST_CASE("const evaluates to a singleton; undef and entry leaves go top",
          "[analysis][image-eval]") {
  Fixture f;
  ImageEval eval(f.function, f.image.reader());

  const ValueSet constant = eval.eval(f.i64(0x1234));
  REQUIRE(!constant.isTop());
  REQUIRE(constant.values().size() == 1);
  CHECK(constant.values()[0] == 0x1234);

  CHECK(eval.eval(f.function.undefined(Type::integer(64))).isTop());
  CHECK(eval.eval(f.function.entryReg(f.function.registers().find("x0"))).isTop());
}

TEST_CASE("a load reads the image; an unmapped load is top, never zero",
          "[analysis][image-eval]") {
  Fixture f;
  f.image.qwords.emplace(0x5000u, 0xCAFEu);
  ImageEval eval(f.function, f.image.reader());

  const BlockId entry = f.block(0x1000);
  const il::ValueId loaded =
      f.function.appendLoad(entry, 0x1000, Type::integer(64), f.i64(0x5000));
  const ValueSet set = eval.eval(f.function.valueRef(loaded));
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 1);
  CHECK(set.values()[0] == 0xCAFE);

  const il::ValueId missing =
      f.function.appendLoad(entry, 0x1004, Type::integer(64), f.i64(0x9999));
  CHECK(eval.eval(f.function.valueRef(missing)).isTop());
}

TEST_CASE("select over an unknown condition unions its arms — the resolution case",
          "[analysis][image-eval]") {
  Fixture f;
  f.image.qwords.emplace(0x6000u, 0xAAAAu);
  f.image.qwords.emplace(0x6008u, 0xBBBBu);
  ImageEval eval(f.function, f.image.reader());

  const BlockId entry = f.block(0x1000);
  // select(cmp.eq(undef, 0), 0x6000, 0x6008), then load: two table entries.
  const ExprId condition = f.function.binary(
      ExprOp::CmpEq, f.function.undefined(Type::integer(64)), f.i64(0));
  const ExprId address =
      f.function.select(condition, f.i64(0x6000), f.i64(0x6008));
  const il::ValueId loaded = f.function.appendLoad(entry, 0x1000, Type::integer(64), address);

  const ValueSet set = eval.eval(f.function.valueRef(loaded));
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 2);
  CHECK(set.values()[0] == 0xAAAA);
  CHECK(set.values()[1] == 0xBBBB);
}

TEST_CASE("arithmetic over small sets cross-products within the cap",
          "[analysis][image-eval]") {
  Fixture f;
  ImageEval eval(f.function, f.image.reader());

  // {1,2} + {10,20} = {11,12,21,22}, via two selects over unknowns.
  const ExprId undefA = f.function.undefined(Type::integer(64));
  const ExprId undefB = f.function.undefined(Type::integer(64));
  const ExprId oneTwo = f.function.select(f.function.binary(ExprOp::CmpEq, undefA, f.i64(0)),
                                          f.i64(1), f.i64(2));
  const ExprId tenTwenty =
      f.function.select(f.function.binary(ExprOp::CmpEq, undefB, f.i64(0)), f.i64(10),
                        f.i64(20));
  const ValueSet set =
      eval.eval(f.function.binary(ExprOp::Add, oneTwo, tenTwenty));
  REQUIRE(!set.isTop());
  CHECK(set.values().size() == 4);
}

TEST_CASE("an EntryReg leaf resolves once EntryRegFacts binds it, and stays top otherwise",
          "[analysis][image-eval]") {
  // Named "x1"/"x2" rather than the real dyld-leaked "x22"/"x28" because the
  // fixture's register file (see il_test_support.h) only models x0..x7 --
  // EntryRegFacts binds by name, so which x-number is used here is
  // otherwise arbitrary.
  Fixture f;
  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromBase("dyld", 0x68310));
  ImageEval eval(f.function, f.image.reader(), &facts);

  // x1 has no binding at all: still top, exactly as with no facts supplied.
  CHECK(eval.eval(f.function.entryReg(f.function.registers().find("x1"))).isTop());

  // x2 is bound, but its companion's base was never given: still top.
  const ExprId x2 = f.function.entryReg(f.function.registers().find("x2"));
  CHECK(eval.eval(x2).isTop());
}

TEST_CASE("an EntryReg leaf resolves to a singleton once its companion base is known",
          "[analysis][image-eval]") {
  Fixture f;
  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromBase("dyld", 0x68310));
  facts.setCompanionBase("dyld", 0x104fe0000);
  ImageEval eval(f.function, f.image.reader(), &facts);

  const ExprId x2 = f.function.entryReg(f.function.registers().find("x2"));
  const ValueSet set = eval.eval(x2);
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 1);
  CHECK(set.values()[0] == 0x104fe0000 + 0x68310);
}

TEST_CASE("a select next to a bare EntryReg arm keeps only the computed one",
          "[analysis][image-eval]") {
  // The absd shape this exists for: one predecessor of a merge point never
  // touched the platform-leaked register at all (so it is still the raw
  // leak there), another predecessor assigned it something real. A select
  // over an unknown condition would ordinarily union both arms -- but a bare
  // EntryReg arm next to a computed one is not "the index is one of these
  // two very different things", it is the untouched predecessor
  // contributing nothing.
  Fixture f;
  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromLiteral(0x104fe0310));
  ImageEval eval(f.function, f.image.reader(), &facts);

  const ExprId entryX2 = f.function.entryReg(f.function.registers().find("x2"));
  const ExprId computed = f.function.select(
      f.function.binary(ExprOp::CmpEq, f.function.undefined(Type::integer(64)), f.i64(0)),
      f.i64(3), f.i64(7));
  const ExprId condition =
      f.function.binary(ExprOp::CmpEq, f.function.undefined(Type::integer(64)), f.i64(1));
  const ExprId merged = f.function.select(condition, entryX2, computed);

  const ValueSet set = eval.eval(merged);
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 2);
  CHECK(std::find(set.values().begin(), set.values().end(), 3) != set.values().end());
  CHECK(std::find(set.values().begin(), set.values().end(), 7) != set.values().end());
  CHECK(std::find(set.values().begin(), set.values().end(), 0x104fe0310) == set.values().end());
}

TEST_CASE("a select over two bare EntryReg arms is an ordinary merge, not a stale one",
          "[analysis][image-eval]") {
  // Both arms untouched: nothing to prefer, so both are kept -- this is not
  // the shape unionEntryRegAware exists to filter.
  Fixture f;
  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromLiteral(0x11));
  facts.setBinding("x3", xdec::analysis::EntryRegBinding::fromLiteral(0x22));
  ImageEval eval(f.function, f.image.reader(), &facts);

  const ExprId entryX2 = f.function.entryReg(f.function.registers().find("x2"));
  const ExprId entryX3 = f.function.entryReg(f.function.registers().find("x3"));
  const ExprId condition =
      f.function.binary(ExprOp::CmpEq, f.function.undefined(Type::integer(64)), f.i64(1));
  const ExprId merged = f.function.select(condition, entryX2, entryX3);

  const ValueSet set = eval.eval(merged);
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 2);
  CHECK(std::find(set.values().begin(), set.values().end(), 0x11) != set.values().end());
  CHECK(std::find(set.values().begin(), set.values().end(), 0x22) != set.values().end());
}

TEST_CASE("a phi mixing a bare EntryReg input with a defined one keeps only the defined one",
          "[analysis][image-eval]") {
  Fixture f;
  xdec::analysis::EntryRegFacts facts;
  facts.setBinding("x2", xdec::analysis::EntryRegBinding::fromLiteral(0x104fe0310));
  ImageEval eval(f.function, f.image.reader(), &facts);

  const BlockId a = f.block(0x1000);
  const BlockId b = f.block(0x1010);
  const BlockId merge = f.block(0x1020);
  f.function.appendBranch(a, 0x1000, merge);
  f.function.appendBranch(b, 0x1010, merge);
  f.function.rebuildEdges();

  const il::ValueId phi = f.function.prependPhi(merge, 0x1020, Type::integer(64));
  const ExprId entryX2 = f.function.entryReg(f.function.registers().find("x2"));
  const std::vector<il::ExprId> inputs{entryX2, f.i64(0x42)};
  f.function.setOperands(f.function.value(phi).definition, inputs);

  const ValueSet set = eval.eval(f.function.valueRef(phi));
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 1);
  CHECK(set.values()[0] == 0x42);
}

TEST_CASE("a phi unions its inputs, and a phi loop concedes gracefully",
          "[analysis][image-eval]") {
  Fixture f;
  ImageEval eval(f.function, f.image.reader());

  const BlockId header = f.block(0x1000);
  const BlockId back = f.block(0x1010);
  f.function.appendBranch(header, 0x1000, back);
  f.function.appendBranch(back, 0x1010, header);
  f.function.rebuildEdges();

  const il::ValueId phi = f.function.prependPhi(header, 0x1000, Type::integer(64));
  // Model a two-input loop phi directly: the constant from one edge, itself
  // from the back edge.
  const std::vector<il::ExprId> inputs{f.i64(0x77), f.function.valueRef(phi)};
  f.function.setOperands(f.function.value(phi).definition, inputs);

  const ValueSet set = eval.eval(f.function.valueRef(phi));
  REQUIRE(!set.isTop());
  REQUIRE(set.values().size() == 1);
  CHECK(set.values()[0] == 0x77);  // the self-edge conceded, the constant won
}

}  // namespace
