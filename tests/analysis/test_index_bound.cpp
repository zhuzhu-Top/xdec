// boundOnIndex: structural bounds proved from a select's own shape, with no
// guard and no dominator to climb -- and the dead-arm refinement that keeps
// one of those selects from claiming a table entry nothing ever reaches.
// preciseIndexSet: the same reasoning carried as values rather than a
// ceiling, so a caller reads a table's live entries and not the gaps below
// them.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/index_bound.h"
#include "xdec/il/function.h"

namespace il = xdec::il;
using xdec::Arch;
using xdec::analysis::boundOnIndex;
using xdec::analysis::Dominators;
using xdec::analysis::preciseIndexSet;
using xdec::il::BlockId;
using xdec::il::ExprId;
using xdec::il::ExprOp;
using xdec::il::Function;
using xdec::il::Type;

namespace {

/// One block ending in an indirect branch: enough for Dominators to compute
/// and enough for boundOnIndex's dominator walk to find no CondBranch guard
/// to climb, so every test here is exercising localBound/armBound's purely
/// structural reasoning, not the guard-climbing half of the contract.
struct Fixture {
  Fixture() : function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {
    entry = function.createBlock(0x1000);
    function.setEntryBlock(entry);
  }

  ExprId i64(uint64_t value) { return function.constant(Type::integer(64), value); }
  ExprId reg(const char* name) {
    return function.entryReg(function.registers().find(name));
  }
  void terminate() {
    function.appendIndirectBranch(entry, 0x1000, i64(0));
    function.rebuildEdges();
  }

  Function function;
  BlockId entry;
};

}  // namespace

TEST_CASE("a zero-extended one-bit flag bounds a select's dead arm out of the total",
          "[analysis][index-bound]") {
  // sub_199214's exact shape: a boolean (here, a comparison standing in for
  // the flag a syscall's errno check produces) zero-extended into i32 then
  // i64 -- localBound already reads that chain as "at most 1" -- selected
  // against a constant 2 on a signed guard that can never actually pick it:
  // `3 <s x` is never true of an `x` that chain has already bounded to 1.
  // Before the dead-arm check, the select's bound was max(2, 1) = 2, one
  // more entry than the table has live cases for.
  Fixture f;
  const ExprId flag = f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0));
  const ExprId widened32 = f.function.cast(ExprOp::ZExt, Type::integer(32), flag);
  const ExprId widened64 = f.function.cast(ExprOp::ZExt, Type::integer(64), widened32);
  const ExprId condition = f.function.binary(ExprOp::CmpLtS, f.i64(3), widened64);
  const ExprId index = f.function.select(condition, f.i64(2), widened64);
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 1);
}

TEST_CASE("a select whose condition can go either way still needs both arms bounded",
          "[analysis][index-bound]") {
  // The soundness check on the dead-arm path: nothing here proves `x2reg`
  // small, so the true arm (a wide, unbounded value) has to count, and the
  // select is unbounded, not accidentally 2.
  Fixture f;
  const ExprId opaque = f.reg("x2");
  const ExprId condition = f.function.binary(ExprOp::CmpLtS, f.i64(3), opaque);
  const ExprId index = f.function.select(condition, opaque, f.i64(2));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  CHECK_FALSE(boundOnIndex(f.function, dominators, f.entry, index).has_value());
}

TEST_CASE("an unsigned-shift-narrowed not, ORed with a literal, bounds the index to their union",
          "[analysis][index-bound]") {
  // absd's own start() shape (see eval/FINDINGS.md's iOS Mach-O entry): a
  // table index built as `(~result >> 31) | 0xa` -- a right shift of 31 on
  // an i32 squeezes *any* operand, known or not, down to {0, 1}, and ORing
  // that onto a literal can only ever land on one of two values, {0xa, 0xb},
  // no matter what the shifted value actually is. `opaque` stands in for
  // that unknowable value (a call's return, in the real sample) so this
  // proves localBound reaches the same conclusion without ever seeing a
  // concrete number for it. Before Or had a case here, the Or node fell
  // through to `default: nullopt`, and ZExt's own type-width fallback then
  // answered 0xffffffff -- a bound so loose it broke resolve-indirect's
  // "every index below a proven bound is a real table entry" assumption,
  // discarding a real 2-entry table over garbage read past its actual end.
  Fixture f;
  const ExprId opaque = f.reg("x0");
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(32), opaque);
  const ExprId inverted = f.function.unary(ExprOp::Not, narrow);
  const ExprId topBit =
      f.function.binary(ExprOp::ShrU, inverted, f.function.constant(Type::integer(32), 31));
  const ExprId tagged =
      f.function.binary(ExprOp::Or, topBit, f.function.constant(Type::integer(32), 0xa));
  const ExprId index = f.function.cast(ExprOp::ZExt, Type::integer(64), tagged);
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 0xb);
}

TEST_CASE("an or of two unbounded operands proves nothing, same as and's own give-up",
          "[analysis][index-bound]") {
  // The symmetric negative case: with neither side a literal, there is no
  // exact bit pattern to combine the other side's bound onto, so (matching
  // And's own refusal a few lines up in index_bound.cpp) this has to give up
  // rather than guess.
  Fixture f;
  const ExprId index = f.function.binary(ExprOp::Or, f.reg("x0"), f.reg("x1"));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  CHECK_FALSE(boundOnIndex(f.function, dominators, f.entry, index).has_value());
}

TEST_CASE("a branchless saturating clamp bounds the index to the clamp's threshold",
          "[analysis][index-bound]") {
  // `state > 5 ? 5 : state`, compiled with CSEL and no branch at all -- the
  // pre-existing readComparison/upperBound path this file had no direct
  // coverage for before.
  Fixture f;
  const ExprId state = f.reg("x3");
  const ExprId condition = f.function.binary(ExprOp::CmpLtU, f.i64(5), state);
  const ExprId index = f.function.select(condition, f.i64(5), state);
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 5);
}

TEST_CASE("a clamp whose live arm is incremented bounds the index one past the clamp",
          "[analysis][index-bound]") {
  // `state > 5 ? 5 : state + 1`, the clamp-and-advance a `cmp`/`csel`/`cinc`
  // trio compiles to. The guard in scope names `state`; the arm computes
  // `state + 1`. readComparison insists on the *same* value, for good reason,
  // so before armBound learned to read a guard through a constant offset the
  // incremented arm fell through to a localBound that had no Add case either,
  // and the whole select was unbounded. An unbounded index is what sends
  // resolve-indirect off scanning a shared table until some entry happens to
  // look wrong, three hundred entries later.
  Fixture f;
  const ExprId state = f.reg("x4");
  const ExprId condition = f.function.binary(ExprOp::CmpLtU, f.i64(5), state);
  const ExprId index =
      f.function.select(condition, f.i64(5), f.function.binary(ExprOp::Add, state, f.i64(1)));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 6);
}

TEST_CASE("a constant offset shifts a masked index's own bound", "[analysis][index-bound]") {
  // The structural half of the same rule, with no comparison involved:
  // `(x & 7) + 1` is at most 8, whatever x is. Before Add had a case, this
  // fell through to `default: nullopt`.
  Fixture f;
  const ExprId masked = f.function.binary(ExprOp::And, f.reg("x0"), f.i64(7));
  const ExprId index = f.function.binary(ExprOp::Add, masked, f.i64(1));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  const auto bound = boundOnIndex(f.function, dominators, f.entry, index);
  REQUIRE(bound.has_value());
  CHECK(*bound == 8);
}

TEST_CASE("an offset that would wrap the type bounds nothing", "[analysis][index-bound]") {
  // The soundness check on the Add rule. A trunc to i8 is at most 0xff, and
  // `+ 1` on that is 0 as readily as it is 0x100, so there is no bound to
  // shift -- claiming 0x100 would be claiming the index reaches an entry a
  // wrapped value never selects, and claiming 0xff would be claiming it never
  // reaches one it does.
  Fixture f;
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(8), f.reg("x0"));
  const ExprId index =
      f.function.binary(ExprOp::Add, narrow, f.function.constant(Type::integer(8), 1));
  f.terminate();

  const Dominators dominators = Dominators::compute(f.function);
  CHECK_FALSE(boundOnIndex(f.function, dominators, f.entry, index).has_value());
}

TEST_CASE("the same shape names its two values, not just their ceiling",
          "[analysis][index-bound]") {
  // Why preciseIndexSet exists. boundOnIndex reads `(~x >> 31) | 0xa` as "at
  // most 0xb", which is true, and a caller enumerating 0..0xb from it reads
  // twelve entries where the expression can only ever select two. On absd's
  // table -- one blob of relative offsets shared by every flattened function
  // in the image -- the ten it reads besides belong to other functions.
  Fixture f;
  const ExprId narrow = f.function.cast(ExprOp::Trunc, Type::integer(32), f.reg("x0"));
  const ExprId inverted = f.function.unary(ExprOp::Not, narrow);
  const ExprId topBit =
      f.function.binary(ExprOp::ShrU, inverted, f.function.constant(Type::integer(32), 31));
  const ExprId tagged =
      f.function.binary(ExprOp::Or, topBit, f.function.constant(Type::integer(32), 0xa));
  const ExprId index = f.function.cast(ExprOp::ZExt, Type::integer(64), tagged);
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{0xa, 0xb});
}

TEST_CASE("a complementary pair buried inside a longer OR chain still collapses to one value",
          "[analysis][index-bound]") {
  // absd's own 8dc shape: three or more OR terms chained (`t26 | t27 | ... |
  // !(t_n != t_m)`), where one pair among them -- not necessarily the two
  // immediate siblings at the top of the tree -- asks the same question on
  // opposite polarities. `a | (!a | b)` is 1 on every path (a and its
  // complement cannot both be 0), but a's complement is nested one level
  // down from a itself, past where the direct two-term BitFactor check a few
  // lines above this one can see it. Before the chain was flattened first,
  // this fell through to the generic cross product and answered the safe
  // but loose {0, 1} -- not wrong, but claiming a table entry (index 0) the
  // branch can never actually select.
  Fixture f;
  const ExprId a =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0)));
  const ExprId notA =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpNe, f.reg("x0"), f.i64(0)));
  const ExprId b =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0)));
  const ExprId index = f.function.binary(ExprOp::Or, a, f.function.binary(ExprOp::Or, notA, b));
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{1});
}

TEST_CASE("an OR chain with no complementary pair at all still resolves via the generic path",
          "[analysis][index-bound]") {
  // The chain-flattening addition has to be strictly additive: three
  // genuinely unrelated flags OR'd together, none of them each other's
  // complement, still answers -- just via the ordinary two-at-a-time
  // recursion, the same as before this shape had its own case.
  Fixture f;
  const ExprId a =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0)));
  const ExprId b =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0)));
  const ExprId c =
      f.function.cast(ExprOp::ZExt, Type::integer(32),
                      f.function.binary(ExprOp::CmpEq, f.reg("x2"), f.i64(0)));
  const ExprId index = f.function.binary(ExprOp::Or, a, f.function.binary(ExprOp::Or, b, c));
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{0, 1});
}

TEST_CASE("a select over two constants is both of them", "[analysis][index-bound]") {
  // The other half of the walk: an arm at a time, unioned. The condition is
  // deliberately unanalysable, because which arm runs does not matter -- both
  // are entries the branch can select, and neither is anything else.
  Fixture f;
  const ExprId condition = f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0));
  const ExprId index = f.function.select(condition, f.i64(3), f.i64(7));
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{3, 7});
}

TEST_CASE("a phi's arms are unioned the same way a select's would be",
          "[analysis][index-bound]") {
  // absd's own 938 shape: a loop-carried dispatcher state (reg:x10) merges a
  // huge one-time seed with a comparison's zero-extended result on the back
  // edge. `exactValues` used to stop at the `val:iN(%k)` wrapper and call the
  // whole thing unknown; reading through to the phi it names, the same way
  // ImageEval's evalValue already does, answers through it instead.
  Fixture f;
  const il::ValueId phi = f.function.prependPhi(f.entry, 0x1000, Type::integer(64));
  const ExprId seed = f.i64(0x1000234b8);
  const ExprId fromCompare =
      f.function.cast(ExprOp::ZExt, Type::integer(64),
                      f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0)));
  f.function.setOperands(f.function.value(phi).definition,
                         std::vector<ExprId>{seed, fromCompare});
  const ExprId index = f.function.valueRef(phi);
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{0, 1, 0x1000234b8});
}

TEST_CASE("a phi mixing a bare EntryReg input with a defined one keeps only the defined one",
          "[analysis][index-bound]") {
  // Same policy as ImageEval::unionEntryRegAware, for the same reason: a
  // platform fact this walk cannot look up (no EntryRegFacts reaches here)
  // should not poison an otherwise small, real merge on the back edge with
  // the one-time seed that only ever flows in on entry.
  Fixture f;
  const il::ValueId phi = f.function.prependPhi(f.entry, 0x1000, Type::integer(64));
  const ExprId entryX2 = f.reg("x2");
  const ExprId fromCompare =
      f.function.cast(ExprOp::ZExt, Type::integer(64),
                      f.function.binary(ExprOp::CmpEq, f.reg("x0"), f.i64(0)));
  f.function.setOperands(f.function.value(phi).definition,
                         std::vector<ExprId>{entryX2, fromCompare});
  const ExprId index = f.function.valueRef(phi);
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{0, 1});
}

TEST_CASE("an index whose shape narrows nothing yields no value set",
          "[analysis][index-bound]") {
  // The soundness boundary. `state + 1` is one value for each value `state`
  // has, and nothing here bounds `state`, so there is no small set to return
  // -- and returning a partial one would license a caller to enumerate it and
  // nothing else. This is the shape absd's second dispatch has (`cinc w10,
  // w19, eq`), and the honest answer for it is that this walk does not know.
  Fixture f;
  const ExprId state = f.reg("x4");
  const ExprId condition = f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0));
  const ExprId index = f.function.select(
      condition, f.function.binary(ExprOp::Add, state, f.i64(1)), state);
  f.terminate();

  CHECK_FALSE(preciseIndexSet(f.function, index).has_value());
}

TEST_CASE("a cinc shape names both values even where wrapping the base costs one "
          "recursion level too many",
          "[analysis][index-bound]") {
  // A `select(cond, state + 1, state)` whose `state` arm on its own just
  // barely fits under exactValues' recursion budget (kMaxDepth, above): the
  // *direct* reference resolves, but the *same* state one Add deeper (inside
  // the `+ 1` arm's own recursive walk) no longer does, one level over
  // budget. Before this shape had its own case, that asymmetry made the
  // whole select fall through to `nullopt` even though the base's values
  // were, in fact, known -- exactly the "known base, unresolved sibling arm"
  // situation armBound's own comment already names for the guard-bound case
  // (a cinc reads the guard through a constant offset the arm adds); this is
  // its value-set counterpart, for a base whose own resolution is already
  // right at the edge of what this walk can afford.
  Fixture f;
  ExprId state = f.i64(5);
  // Eleven Add-by-zero wrappers: resolving `state` from the select's arms
  // (one call frame in) costs exactly up to depth 12, the cap; resolving it
  // from *inside* the `+ 1` arm's own Add case (two frames in) costs 13 and
  // is refused.
  for (int i = 0; i < 11; ++i) {
    state = f.function.binary(ExprOp::Add, state, f.i64(0));
  }
  const ExprId condition = f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0));
  const ExprId index = f.function.select(
      condition, f.function.binary(ExprOp::Add, state, f.i64(1)), state);
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{5, 6});
}

TEST_CASE("a csinc-style select with the base on the true edge also names both values",
          "[analysis][index-bound]") {
  // Same asymmetry, arms swapped: the base runs on the true edge and the
  // increment on the false edge, exercising the other direction of the
  // known/unknown-arm search.
  Fixture f;
  ExprId state = f.i64(10);
  for (int i = 0; i < 11; ++i) {
    state = f.function.binary(ExprOp::Add, state, f.i64(0));
  }
  const ExprId condition = f.function.binary(ExprOp::CmpEq, f.reg("x1"), f.i64(0));
  const ExprId index =
      f.function.select(condition, state, f.function.binary(ExprOp::Add, state, f.i64(1)));
  f.terminate();

  const auto values = preciseIndexSet(f.function, index);
  REQUIRE(values.has_value());
  CHECK(*values == std::vector<uint64_t>{10, 11});
}
