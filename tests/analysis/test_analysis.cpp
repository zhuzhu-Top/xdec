// The analysis library: dominators, post-dominators, SCCs, loops,
// reducibility.
//
// Every graph here is built block-by-block on purpose: the analyses are the
// foundation the rest of the pipeline stands on, and a helper that built the
// graph wrong would pass the test while the analysis was broken, or vice
// versa. Hand-laid edges keep the expected answers obvious.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

#include "il/il_test_support.h"
#include "xdec/analysis/dominators.h"
#include "xdec/analysis/loops.h"
#include "xdec/analysis/scc.h"
#include "xdec/il/function.h"

using xdec::Arch;
using xdec::analysis::BackEdge;
using xdec::analysis::Dominators;
using xdec::analysis::NaturalLoop;
using xdec::analysis::PostDominators;
using xdec::analysis::Sccs;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;

namespace {

/// A bare CFG builder: blocks at 0x1000 + 4*i, edges by terminator append.
class Cfg {
 public:
  Cfg() : function_(Arch::AArch64, xdec::test::arm64Registers(), 0x1000) {}

  BlockId add() {
    const BlockId block = function_.createBlock(0x1000 + 4 * next_++);
    if (!function_.entryBlock().valid()) {
      function_.setEntryBlock(block);
    }
    return block;
  }

  void link(BlockId from, BlockId to) {
    function_.appendBranch(from, function_.block(from).va, to);
  }

  void fork(BlockId from, BlockId taken, BlockId fallthrough) {
    const xdec::il::RegId x0 = function_.registers().find("x0");
    const xdec::il::ValueId cond = function_.appendReadReg(from, function_.block(from).va, x0);
    function_.appendCondBranch(from, function_.block(from).va, function_.valueRef(cond),
                               taken, fallthrough);
  }

  void end(BlockId block) { function_.appendReturn(block, function_.block(block).va); }

  Function& finish() {
    function_.rebuildEdges();
    return function_;
  }

 private:
  Function function_;
  uint32_t next_ = 0;
};

[[nodiscard]] bool contains(std::span<const BlockId> blocks, BlockId needle) {
  return std::find(blocks.begin(), blocks.end(), needle) != blocks.end();
}

//        a
//      /   +
//     b     c
//      +   /
//        d
TEST_CASE("dominators over a diamond", "[analysis][dom]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  cfg.fork(a, b, c);
  cfg.link(b, d);
  cfg.link(c, d);
  cfg.end(d);
  const Function& function = cfg.finish();

  const Dominators dom = Dominators::compute(function);
  REQUIRE(dom.rpo().size() == 4);

  CHECK(dom.idom(a) == BlockId{});
  CHECK(dom.idom(b) == a);
  CHECK(dom.idom(c) == a);
  CHECK(dom.idom(d) == a);

  CHECK(dom.dominates(a, d));
  CHECK_FALSE(dom.dominates(b, c));
  CHECK_FALSE(dom.dominates(b, d));  // b does not dominate d: c is another way in
  CHECK(dom.strictlyDominates(a, b));
  CHECK(dom.dominates(b, b));  // reflexive

  // The join is in the frontier of both arms and of nothing else.
  CHECK(dom.frontier(b) == std::set<BlockId>{d});
  CHECK(dom.frontier(c) == std::set<BlockId>{d});
  CHECK(dom.frontier(d).empty());
}

//        a
//      /   +
//     b     c
//      +   /
//        d
//      /   +
//     e     f   <- two exits: the virtual root case
TEST_CASE("post-dominators with two exits meet at the virtual root", "[analysis][pdom]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  const BlockId e = cfg.add();
  const BlockId f = cfg.add();
  cfg.fork(a, b, c);
  cfg.link(b, d);
  cfg.link(c, d);
  cfg.fork(d, e, f);
  cfg.end(e);
  cfg.end(f);
  const Function& function = cfg.finish();

  const PostDominators pdom = PostDominators::compute(function);
  for (const BlockId block : {a, b, c, d, e, f}) {
    REQUIRE(pdom.reachesExit(block));
  }

  CHECK(pdom.ipdom(e) == BlockId{});  // directly under the virtual exit root
  CHECK(pdom.ipdom(f) == BlockId{});
  CHECK(pdom.ipdom(d) == BlockId{});  // e and f meet only at the root
  CHECK(pdom.ipdom(b) == d);
  CHECK(pdom.ipdom(c) == d);
  CHECK(pdom.ipdom(a) == d);

  CHECK(pdom.postDominates(d, a));
  CHECK_FALSE(pdom.postDominates(e, a));
}

//   a -> b <-> c -> d   (b,c form a 2-cycle; a and d are singletons)
TEST_CASE("tarjan finds the cycle and orders sinks first", "[analysis][scc]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  cfg.link(a, b);
  cfg.link(b, c);
  cfg.fork(c, b, d);  // cycle edge and exit edge
  cfg.end(d);
  const Function& function = cfg.finish();

  const Sccs sccs = Sccs::compute(function);
  REQUIRE(sccs.components().size() == 3);

  // Reverse topological: {d} completes first, then {b,c}, then {a}.
  const auto& first = sccs.components()[0];
  CHECK(first.blocks == std::vector<BlockId>{d});
  CHECK_FALSE(first.cyclic);

  const auto& cycle = sccs.components()[1];
  CHECK(cycle.cyclic);
  REQUIRE(cycle.blocks.size() == 2);
  CHECK(contains(cycle.blocks, b));
  CHECK(contains(cycle.blocks, c));

  CHECK(sccs.componentOf(d) == 0);
  CHECK(sccs.componentOf(b) == sccs.componentOf(c));
}

//   a -> a (self loop)
TEST_CASE("a self-loop is a cyclic singleton component", "[analysis][scc]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  cfg.fork(a, a, a);
  const Function& function = cfg.finish();

  const Sccs sccs = Sccs::compute(function);
  REQUIRE(sccs.components().size() == 1);
  CHECK(sccs.components()[0].cyclic);
}

//      a
//      v
//      b <--+
//     /  +  |
//    c    d |
//     +  /  |
//      e ---+
TEST_CASE("back edges and the natural loop around a header", "[analysis][loop]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  const BlockId e = cfg.add();
  cfg.link(a, b);
  cfg.fork(b, c, d);
  cfg.link(c, e);
  cfg.link(d, e);
  cfg.link(e, b);
  const Function& function = cfg.finish();

  const Dominators dom = Dominators::compute(function);
  const std::vector<BackEdge> edges = backEdges(function, dom);
  REQUIRE(edges.size() == 1);
  CHECK(edges[0].from == e);
  CHECK(edges[0].header == b);

  const std::vector<NaturalLoop> loops = naturalLoops(function, dom);
  REQUIRE(loops.size() == 1);
  const NaturalLoop& loop = loops.front();
  CHECK(loop.header == b);
  CHECK(loop.blocks == std::set<BlockId>{b, c, d, e});
  CHECK(loop.latches == std::vector<BlockId>{e});

  CHECK(isReducible(function, Sccs::compute(function)));
}

//   a --> b
//   |     |
//   v     v
//   c <-> d    two entries into the cycle: irreducible
TEST_CASE("a cycle with two entries is irreducible", "[analysis][loop]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId b = cfg.add();
  const BlockId c = cfg.add();
  const BlockId d = cfg.add();
  cfg.fork(a, c, b);
  cfg.link(b, d);
  cfg.link(c, d);
  cfg.link(d, c);
  const Function& function = cfg.finish();

  const Sccs sccs = Sccs::compute(function);
  // Sanity: {c,d} is the only cycle.
  unsigned cyclic = 0;
  for (const auto& component : sccs.components()) {
    cyclic += component.cyclic ? 1 : 0;
  }
  REQUIRE(cyclic == 1);

  CHECK_FALSE(isReducible(function, sccs));
  // And the dominator view agrees there is no back edge: neither c nor d
  // dominates the other.
  const Dominators dom = Dominators::compute(function);
  CHECK(backEdges(function, dom).empty());
}

TEST_CASE("unreachable blocks are outside every judgement", "[analysis][dom]") {
  Cfg cfg;
  const BlockId a = cfg.add();
  const BlockId dead = cfg.add();
  cfg.end(a);
  cfg.end(dead);
  const Function& function = cfg.finish();

  const Dominators dom = Dominators::compute(function);
  CHECK(dom.reachable(a));
  CHECK_FALSE(dom.reachable(dead));
  CHECK(dom.dominates(dead, dead));       // only itself
  CHECK_FALSE(dom.dominates(dead, a));    // and nothing else
  CHECK(dom.depth(dead) == -1);
}

}  // namespace
