// Switch recovery from a decision tree over one value.
//
// Two very different-looking shapes lower to the same thing. A compiler turns a
// sparse `switch` into a binary search: range tests narrow the value down, then
// equality tests pick the case, and every value no case names falls to one
// shared block. Control-flow flattening produces the degenerate form of exactly
// that tree — a run of equality tests on a state variable, one per original
// block, each falling through to the next.
//
// Both are one switch, and printing them as one is the difference between a
// reader seeing "dispatch on this value" and seeing a ladder of anonymous
// compare-and-goto pairs whose shared structure they have to reconstruct by
// hand. This does not undo flattening: the state is still computed at runtime.
// It makes the shape legible.
#include <algorithm>
#include <limits>
#include <numeric>

#include "structurizer.h"

namespace xdec::emit {

namespace {

/// Whether every edge in `preds` into `target` carries the same phi copies. A
/// switch has one `default:` arm, so tests that all fall through to the default
/// may only be merged into it when they agree on what that edge transfers.
bool edgesAgree(const il::Function& function, il::BlockId target,
                const std::vector<il::BlockId>& preds) {
  const il::Block& block = function.block(target);
  const auto& predecessors = block.predecessors;
  const auto slotOf = [&predecessors](il::BlockId pred) {
    const auto found = std::find(predecessors.begin(), predecessors.end(), pred);
    return found == predecessors.end()
               ? std::numeric_limits<std::size_t>::max()
               : static_cast<std::size_t>(found - predecessors.begin());
  };
  for (const il::OpId opId : block.ops) {
    const il::Op& op = function.op(opId);
    if (op.code != il::OpCode::Phi) {
      break;  // phis lead the block
    }
    const auto operands = function.operands(op);
    std::optional<il::ExprId> shared;
    for (const il::BlockId pred : preds) {
      const std::size_t slot = slotOf(pred);
      if (slot >= operands.size()) {
        return false;
      }
      if (shared.has_value() && *shared != operands[slot]) {
        return false;
      }
      shared = operands[slot];
    }
  }
  return true;
}

/// A `Stmt::make(StmtKind::Continue)` naming `header`, for a case/default arm
/// whose only mention of it is the bare `BlockId` `StmtPrinter::printSwitch`
/// prints as a `goto` when it finds no body of its own -- there is no `Goto`
/// node there for `continueAtBackEdges` to find and flip, so this gives it
/// one that prints the same way `Continue` already does everywhere else:
/// `printEdge` for that case's own edge runs first either way (see
/// `printSwitch`), and `Continue`'s own `consumePending` is a no-op right
/// after, since `printSwitch` always clears `pending_` immediately before
/// printing whatever this replaces.
StmtPtr continueTo(il::BlockId header) {
  auto stmt = Stmt::make(StmtKind::Continue);
  stmt->block = header;
  return stmt;
}

}  // namespace

/// Turns every jump back to `header` inside `node` into a `continue`, and says
/// whether it found any. Nested loops are left alone: a `continue` written
/// inside one belongs to it, not to the loop being built here. A `Switch`
/// case/default arm with no body of its own names its target as a bare
/// `BlockId` instead of a `Goto` node (see `StmtPrinter::printSwitch`'s own
/// fallback) -- given one here via `continueTo`, exactly as if `claimCaseBody`
/// had handed it a body all along. Shared with structure.cpp's
/// `collapseLabeledNaturalLoops` (J2f) -- both callers hand this the
/// *finished* body of a loop that has already claimed every block it will
/// ever contain, so there is nothing here that needs to know which of the two
/// built that body.
bool Structurizer::continueAtBackEdges(Stmt* node, il::BlockId header) {
  if (node == nullptr) {
    return false;
  }
  if (node->kind == StmtKind::Goto && node->block == header) {
    node->kind = StmtKind::Continue;
    return true;
  }
  if (node->kind == StmtKind::While || node->kind == StmtKind::DoWhile) {
    return false;
  }
  bool found = false;
  if (node->kind == StmtKind::Switch) {
    for (std::size_t index = 0; index < node->cases.size(); ++index) {
      if (index < node->caseBodies.size() && node->caseBodies[index]) {
        found |= continueAtBackEdges(node->caseBodies[index].get(), header);
      } else if (node->cases[index] == header) {
        if (index >= node->caseBodies.size()) {
          node->caseBodies.resize(node->cases.size());
        }
        node->caseBodies[index] = continueTo(header);
        found = true;
      }
    }
    if (node->defaultBody) {
      found |= continueAtBackEdges(node->defaultBody.get(), header);
    } else if (node->defaultCase == header) {
      node->defaultBody = continueTo(header);
      found = true;
    }
    found |= continueAtBackEdges(node->epilogue.get(), header);
    return found;
  }
  for (const StmtPtr& item : node->items) {
    found |= continueAtBackEdges(item.get(), header);
  }
  found |= continueAtBackEdges(node->thenArm.get(), header);
  found |= continueAtBackEdges(node->elseArm.get(), header);
  // J2e-if: an `If`'s own epilogue (see switchFor's 2-way collapse) is
  // exactly as capable of holding a back edge as a Switch's epilogue is.
  found |= continueAtBackEdges(node->epilogue.get(), header);
  return found;
}

std::optional<Structurizer::ValueTest> Structurizer::matchValueTest(
    il::BlockId blockId, bool allowPhis) const {
  const auto& ops = function_.block(blockId).ops;
  const il::Op* terminator = terminatorOf(blockId);
  if (terminator == nullptr || terminator->code != il::OpCode::CondBranch) {
    return std::nullopt;
  }
  // Nothing but phis may precede the compare: a body would be lost, and a phi
  // in a block further down the tree would lose its incoming copies, because
  // the switch leaves no place for the transfer into that block.
  for (const il::OpId opId : ops) {
    const il::Op& op = function_.op(opId);
    if (op.code == il::OpCode::Phi) {
      if (!allowPhis) {
        return std::nullopt;
      }
      continue;
    }
    if (&op != terminator) {
      return std::nullopt;
    }
  }

  const il::Expr& cond = function_.expr(function_.operands(*terminator)[0]);
  const auto targets = function_.targets(*terminator);
  ValueTest result;
  uint64_t value = 0;
  switch (cond.op) {
    case il::ExprOp::CmpEq:
    case il::ExprOp::CmpNe: {
      if (!function_.asConstant(cond.operands[1], value)) {
        return std::nullopt;
      }
      const bool invert = cond.op == il::ExprOp::CmpNe;
      result.equality = true;
      result.spine = cond.operands[0];
      result.value = value;
      result.taken = targets[invert ? 1 : 0];
      result.next = targets[invert ? 0 : 1];
      return result;
    }
    case il::ExprOp::CmpLtU:
    case il::ExprOp::CmpLeU:
    case il::ExprOp::CmpLtS:
    case il::ExprOp::CmpLeS: {
      // Either side may hold the constant: `x < 4` and `4 < x` are both
      // ordinary halves of a binary search over x. Which half an arm covers
      // does not matter here — the walk only needs to know that both arms
      // still test the same value, and duplicate case values are rejected
      // outright, so an arm cannot smuggle in a value another one claimed.
      if (function_.asConstant(cond.operands[1], value)) {
        result.spine = cond.operands[0];
      } else if (function_.asConstant(cond.operands[0], value)) {
        result.spine = cond.operands[1];
      } else {
        return std::nullopt;
      }
      result.taken = targets[0];
      result.next = targets[1];
      return result;
    }
    default:
      return std::nullopt;
  }
}

bool Structurizer::collectTree(il::BlockId head, ValueTree& tree) const {
  const std::optional<ValueTest> root = matchValueTest(head, true);
  if (!root.has_value()) {
    return false;
  }
  tree.spine = root->spine;
  std::set<uint64_t> seen;
  // Each entry is a block that would have to be another test for the tree to
  // keep growing, together with the test it was reached from.
  std::vector<std::pair<il::BlockId, il::BlockId>> pending{{head, il::BlockId{}}};
  while (!pending.empty()) {
    const auto [block, parent] = pending.back();
    pending.pop_back();
    const bool isRoot = block == head;
    // A block joins the tree only if this tree is its sole entry: anything else
    // is a shared region, and replacing it with a case would strand its other
    // predecessors' edges. The root is the exception — the region walk arrives
    // at it the same way it arrives at any other block.
    const bool available =
        block.valid() &&
        (isRoot || (!emitted_.contains(block) && !inProgressHeaders_.contains(block) &&
                    function_.block(block).predecessors.size() == 1 &&
                    function_.block(block).predecessors.front() == parent));
    const std::optional<ValueTest> test =
        available ? matchValueTest(block, isRoot) : std::nullopt;
    if (!test.has_value() || test->spine != tree.spine) {
      // Not a test on the discriminant, so this is where the values no case
      // names end up. There is only one `default:` to put them in.
      if (!block.valid() || (tree.defaultTarget.valid() && tree.defaultTarget != block)) {
        return false;
      }
      tree.defaultTarget = block;
      tree.defaultPreds.push_back(parent);
      continue;
    }
    tree.absorbed.push_back(block);
    if (test->equality) {
      if (!seen.insert(test->value).second) {
        return false;  // two cases for one value: this is not one switch
      }
      tree.values.push_back(test->value);
      tree.handlers.push_back(test->taken);
      tree.preds.push_back(block);
      pending.emplace_back(test->next, block);
    } else {
      pending.emplace_back(test->taken, block);
      pending.emplace_back(test->next, block);
    }
  }
  // A default landing inside the tree would be a jump into the middle of the
  // tests the switch replaced. Only the root survives as a jumpable label — it
  // is the switch itself, and a dispatcher's back edge legitimately lands there.
  if (!tree.defaultTarget.valid() ||
      (tree.defaultTarget != head &&
       std::find(tree.absorbed.begin(), tree.absorbed.end(), tree.defaultTarget) !=
           tree.absorbed.end())) {
    return false;
  }
  return edgesAgree(function_, tree.defaultTarget, tree.defaultPreds);
}

bool Structurizer::tryDispatchTree(Stmt* seq, il::BlockId head, unsigned depth) {
  ValueTree tree;
  if (!collectTree(head, tree) || tree.values.size() < 3) {
    return false;  // two compares are a nested diamond, not a dispatcher
  }
  for (const il::BlockId block : tree.absorbed) {
    mark(block);
  }

  // The tests come in the order a binary search visits them, which is not an
  // order anyone reads a switch in.
  std::vector<std::size_t> order(tree.values.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&tree](std::size_t lhs, std::size_t rhs) {
    return tree.values[lhs] < tree.values[rhs];
  });

  auto stmt = Stmt::make(StmtKind::Switch);
  stmt->block = head;
  stmt->cond = tree.spine;
  for (const std::size_t index : order) {
    stmt->caseValues.push_back(tree.values[index]);
    stmt->cases.push_back(tree.handlers[index]);
    stmt->casePreds.push_back(tree.preds[index]);
    // A handler this tree reaches and nothing else does belongs inside its case.
    // Left outside, the switch says only which label each value jumps to, and a
    // reader has to hold seven labels in mind and go looking for each one to
    // find out what the switch actually does.
    stmt->caseBodies.push_back(
        claimCaseBody(tree.preds[index], tree.handlers[index], depth));
    if (!stmt->caseBodies.back()) {
      addGotoTarget(tree.handlers[index]);
    }
  }
  stmt->defaultCase = tree.defaultTarget;
  stmt->defaultPred = tree.defaultPreds.front();
  stmt->defaultBody = claimCaseBody(stmt->defaultPred, tree.defaultTarget, depth);
  if (!stmt->defaultBody) {
    addGotoTarget(tree.defaultTarget);
  }
  seq->items.push_back(wrapAsLoop(std::move(stmt), head));
  return true;
}

StmtPtr Structurizer::wrapAsLoop(StmtPtr stmt, il::BlockId head) {
  // A state machine is a switch its own cases come back to. Said as a switch
  // alone, every case ends in a jump to a label above it and the reader has to
  // work out that the label is the top of a loop; said as a loop, the cases say
  // `continue` and the shape is the shape of the source.
  //
  // Nothing can fall out of the bottom: every case body was claimed only if
  // control leaves it, an unclaimed one jumps to its handler, and the default
  // arm always exists — so `while (true)` is not hiding an exit, it is the only
  // way out being a return or a jump.
  if (!loopByHeader_.contains(head) || !continueAtBackEdges(stmt.get(), head)) {
    return stmt;
  }
  auto loop = Stmt::make(StmtKind::While);
  loop->block = head;
  loop->body = std::move(stmt);
  return loop;
}

}  // namespace xdec::emit
