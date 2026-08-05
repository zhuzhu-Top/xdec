// A function's IL: blocks of ops over a hash-consed expression pool.
//
// Storage is flat vectors addressed by strong handles. Nothing holds a pointer
// into another container, so appending never invalidates anything, the whole
// structure serialises trivially, and a handle printed in a dump is a stable
// name you can search for across runs.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xdec/il/expr.h"
#include "xdec/il/maturity.h"
#include "xdec/il/op.h"
#include "xdec/il/register_file.h"
#include "xdec/il/type.h"
#include "xdec/support/handle.h"
#include "xdec/support/target.h"

namespace xdec::il {

struct Block {
  /// Address of the first machine instruction in the block.
  uint64_t va = 0;
  /// One past the last machine instruction; equals `va` for synthesised blocks.
  uint64_t endVa = 0;
  std::vector<OpId> ops;
  /// Derived from the terminator by rebuildEdges(); the verifier checks that
  /// they still agree.
  std::vector<BlockId> successors;
  std::vector<BlockId> predecessors;
  /// Lives outside this function: a boundary stub standing for a direct branch
  /// target the lifter could not follow (a tail call into the unmapped, say).
  /// Stubs carry their address and nothing else, so edges to them resolve to
  /// real BlockIds; the verifier exempts them from the empty-block and
  /// terminator rules at every level.
  bool external = false;

  [[nodiscard]] bool empty() const noexcept { return ops.empty(); }
  [[nodiscard]] OpId terminator() const noexcept {
    return ops.empty() ? OpId::invalid() : ops.back();
  }
};

class Function {
 public:
  Function(Arch arch, const RegisterFile& registers, uint64_t entryVa);

  Function(const Function&) = delete;
  Function& operator=(const Function&) = delete;
  Function(Function&&) = default;
  Function& operator=(Function&&) = delete;

  // -- metadata -------------------------------------------------------------

  [[nodiscard]] Arch arch() const noexcept { return arch_; }
  [[nodiscard]] const RegisterFile& registers() const noexcept { return *registers_; }
  [[nodiscard]] uint64_t entryVa() const noexcept { return entryVa_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void setName(std::string name) { name_ = std::move(name); }
  [[nodiscard]] Maturity maturity() const noexcept { return maturity_; }
  void setMaturity(Maturity maturity) noexcept { maturity_ = maturity; }
  [[nodiscard]] BlockId entryBlock() const noexcept { return entryBlock_; }
  void setEntryBlock(BlockId block) noexcept { entryBlock_ = block; }

  // -- expressions ----------------------------------------------------------
  //
  // All builders return an interned handle: constructing the same expression
  // twice yields the same ExprId.

  ExprId constant(Type type, uint64_t value);
  ExprId boolean(bool value);
  ExprId valueRef(ValueId value);
  ExprId undefined(Type type);
  /// The value `reg` held at function entry. SSA construction seeds every
  /// tracked register with one of these instead of an undef, so the stack
  /// pointer's origin stays distinguishable from every other unknown input.
  ExprId entryReg(RegId reg);
  ExprId unary(ExprOp op, ExprId operand);
  ExprId binary(ExprOp op, ExprId lhs, ExprId rhs);
  /// ZExt, SExt, Trunc, Bitcast and the float conversions.
  ExprId cast(ExprOp op, Type type, ExprId operand);
  ExprId extract(Type type, ExprId operand, unsigned lowBit);
  ExprId concat(Type type, ExprId high, ExprId low);
  ExprId select(ExprId condition, ExprId ifTrue, ExprId ifFalse);
  /// Produces the opaque flag bundle. Nothing is expanded here: that laziness is
  /// what lets a later constant fold collapse an entire opaque predicate.
  ExprId flagDef(FlagOp op, unsigned width, std::span<const ExprId> operands);
  ExprId flagCondition(ExprId flags, ConditionCode code);
  ExprId flagBitOf(ExprId flags, FlagBitIndex bit);
  /// Generic entry point; prefer the typed builders.
  ExprId intern(const Expr& expr);

  [[nodiscard]] const Expr& expr(ExprId id) const { return exprs_[id]; }
  [[nodiscard]] std::size_t exprCount() const noexcept { return exprs_.size(); }
  [[nodiscard]] bool hasExpr(ExprId id) const noexcept { return exprs_.contains(id); }
  [[nodiscard]] auto exprHandles() const { return exprs_.handles(); }

  /// The constant an expression denotes, when it is a Const node.
  [[nodiscard]] bool asConstant(ExprId id, uint64_t& out) const;

  /// The constant behind an expression, seeing through the width adjustments a
  /// value picks up on its way between registers.
  ///
  /// `mov w8, #64` writes the low half of x8, so a 64-bit read of it is a
  /// zero-extension of a 32-bit constant, and a value passed through a
  /// narrower parameter arrives truncated. Neither changes which constant is
  /// meant, and both are common enough that a caller asking "is this a known
  /// number" would otherwise get "no" for ordinary code. The returned value is
  /// the innermost constant, untruncated: use it where the identity of the
  /// number matters, not where its exact bit pattern at this width does.
  [[nodiscard]] bool asConstantThroughCasts(ExprId id, uint64_t& out) const;

  // -- blocks ---------------------------------------------------------------

  BlockId createBlock(uint64_t va);
  [[nodiscard]] Block& block(BlockId id) { return blocks_[id]; }
  [[nodiscard]] const Block& block(BlockId id) const { return blocks_[id]; }
  [[nodiscard]] std::size_t blockCount() const noexcept { return blocks_.size(); }
  [[nodiscard]] bool hasBlock(BlockId id) const noexcept { return blocks_.contains(id); }
  [[nodiscard]] auto blockHandles() const { return blocks_.handles(); }
  /// The block starting exactly at `va`, if any.
  [[nodiscard]] BlockId blockAt(uint64_t va) const;

  // -- operations -----------------------------------------------------------
  //
  // Each appender takes the source instruction address, because provenance is
  // not optional: there is no overload that omits it.

  ValueId appendReadReg(BlockId block, uint64_t va, RegId reg);
  OpId appendWriteReg(BlockId block, uint64_t va, RegId reg, ExprId value);
  ValueId appendLoad(BlockId block, uint64_t va, Type type, ExprId address);
  OpId appendStore(BlockId block, uint64_t va, Type type, ExprId address, ExprId value);
  OpId appendBranch(BlockId block, uint64_t va, BlockId target);
  OpId appendCondBranch(BlockId block, uint64_t va, ExprId condition, BlockId ifTrue,
                        BlockId ifFalse);
  /// Targets start out empty; the resolution phase fills them in via setTargets.
  OpId appendIndirectBranch(BlockId block, uint64_t va, ExprId target);
  /// A call defines a value when resultType is non-void: the calling
  /// convention's result register (x0 on AArch64) after the call. SSA
  /// construction binds that version to the result register instead of the
  /// unknown it uses for registers the call merely clobbers.
  OpId appendCall(BlockId block, uint64_t va, ExprId target,
                  Type resultType = Type::voidType());
  OpId appendReturn(BlockId block, uint64_t va);
  OpId appendNop(BlockId block, uint64_t va);
  OpId appendUnreachable(BlockId block, uint64_t va);
  /// Records that the instruction at `va` could not be lifted, naming it.
  OpId appendUnimplemented(BlockId block, uint64_t va, std::string_view mnemonic);
  /// An operation whose effect is identified but not modelled. `resultType` may
  /// be void for a pure side effect.
  OpId appendIntrinsic(BlockId block, uint64_t va, std::string_view name, Type resultType,
                       std::span<const ExprId> arguments);
  OpId appendPhi(BlockId block, uint64_t va, Type type, std::span<const ExprId> incoming);
  /// A phi with no operands yet, inserted at the block's head: SSA
  /// construction places phis before it knows the renaming, and phis must
  /// lead the block. Fill the incoming values with setOperands, one per
  /// predecessor, in predecessor order.
  ValueId prependPhi(BlockId block, uint64_t va, Type type);

  [[nodiscard]] Op& op(OpId id) { return ops_[id]; }
  [[nodiscard]] const Op& op(OpId id) const { return ops_[id]; }
  [[nodiscard]] std::size_t opCount() const noexcept { return ops_.size(); }
  [[nodiscard]] bool hasOp(OpId id) const noexcept { return ops_.contains(id); }

  /// Views into the shared pools. Like any reference into this class, they are
  /// invalidated by a subsequent append; re-fetch through the handle rather than
  /// holding one across an edit.
  [[nodiscard]] std::span<const ExprId> operands(const Op& op) const;
  [[nodiscard]] std::span<const BlockId> targets(const Op& op) const;
  /// Replaces a terminator's successor list.
  void setTargets(OpId id, std::span<const BlockId> targets);

  /// Replaces an op's operand list. Needed where operands cannot be known when
  /// the op is created, such as phi nodes whose incoming values are defined in
  /// blocks that have not been built yet.
  void setOperands(OpId id, std::span<const ExprId> operands);

  /// The value an op defines, or an invalid handle.
  [[nodiscard]] ValueId resultOf(OpId id) const;

  /// Removes an op from its block. Preconditions, asserted: the op is in the
  /// block, is not a terminator, and — when it defines a value — the caller has
  /// established the value has no remaining uses. The op slot stays in the
  /// pool (handles are stable), tombstoned: its result is cleared, and the
  /// value's definition is invalidated, which is how the verifier tells a
  /// removed value from a corrupt one.
  void removeOp(BlockId block, OpId op);

  /// Drops a block's terminator, leaving the block without one so that a
  /// different exit can be appended in its place.
  ///
  /// The state in between is invalid IL — a block must end in a terminator —
  /// so this is only for a rewrite that puts one back before anything else
  /// looks. It exists because replacing a block's exit is a real
  /// transformation (an indirect branch that turns out to be a tail call
  /// becomes a call and a return) and the alternative, mutating an op's code in
  /// place, cannot give the new op the result value a call needs.
  void dropTerminator(BlockId block);

  // -- values ---------------------------------------------------------------

  [[nodiscard]] const ValueInfo& value(ValueId id) const { return values_[id]; }
  [[nodiscard]] std::size_t valueCount() const noexcept { return values_.size(); }
  [[nodiscard]] bool hasValue(ValueId id) const noexcept { return values_.contains(id); }
  [[nodiscard]] auto valueHandles() const { return values_.handles(); }

  // -- names ----------------------------------------------------------------

  uint32_t internName(std::string_view name);
  [[nodiscard]] std::string_view nameOf(uint32_t id) const;

  /// Registers a pass name so that provenance round-trips as text.
  PassId internPass(std::string_view name);
  [[nodiscard]] std::string_view passName(PassId id) const;
  [[nodiscard]] std::size_t passCount() const noexcept { return passNames_.size(); }

  /// Pass identity applied to every op appended from now on. Passes set this
  /// once instead of threading it through every call.
  void setCurrentPass(PassId pass) noexcept { currentPass_ = pass; }
  [[nodiscard]] PassId currentPass() const noexcept { return currentPass_; }

  // -- annotations ----------------------------------------------------------
  //
  // What a pass learned about an op but cannot express as IL. An analysis that
  // proves an indirect call's target rewrites the target and needs no note; one
  // that only recognises the *shape* of a dispatch — "this is a table lookup
  // whose index is a runtime value" — has established something a reader wants
  // and the IL has no way to say. Before this channel existed the choice was to
  // invent an unproven rewrite or to discard the finding; a note keeps it
  // without either.
  //
  // Deliberately a side table rather than an Op field: notes are rare, so a
  // string per op would be mostly waste, and keeping them out of Op means
  // nothing that reasons about semantics can accidentally read one. Deliberately
  // prose rather than an enum: the audience is a human reading emitted C, and an
  // enum of every observation a future pass might make is a taxonomy no one can
  // finish.

  /// Attaches `note` to `op`, replacing any previous one; an empty note clears
  /// it. Notes never affect semantics: every consumer is free to ignore them,
  /// and dropping all of them changes nothing about what the function computes.
  /// One line of prose — the text form prints a note on the op's own line, so a
  /// newline in one would produce a dump that does not parse back.
  void annotate(OpId op, std::string note);
  /// Adds `note` after any note already there, separated by `; `. Two passes can
  /// each learn something about one op — what a call's target is, how many
  /// arguments it takes — and neither has grounds to erase the other's finding.
  void appendNote(OpId op, std::string_view note);
  /// The note on `op`, or empty if it has none.
  [[nodiscard]] std::string_view noteOn(OpId op) const;
  [[nodiscard]] const std::unordered_map<OpId, std::string>& notes() const noexcept {
    return notes_;
  }

  // -- CFG ------------------------------------------------------------------

  /// Recomputes successor and predecessor lists from block terminators. Cheap
  /// enough to run after any structural edit, and the verifier requires the
  /// stored edges to match what this would produce.
  void rebuildEdges();

  /// Blocks in reverse post-order from the entry. Unreachable blocks are
  /// excluded, and reported separately by the verifier.
  [[nodiscard]] std::vector<BlockId> reversePostOrder() const;

 private:
  OpId appendOp(BlockId block, Op op, std::span<const ExprId> operands,
                std::span<const BlockId> targets);
  ValueId defineValue(Type type, OpId definition, BlockId block);

  Arch arch_ = Arch::Unknown;
  const RegisterFile* registers_ = nullptr;
  uint64_t entryVa_ = 0;
  std::string name_;
  Maturity maturity_ = Maturity::Lifted;
  BlockId entryBlock_;
  PassId currentPass_ = kPassLifter;

  HandleVector<ExprId, Expr> exprs_;
  std::unordered_map<Expr, ExprId> exprIndex_;

  HandleVector<OpId, Op> ops_;
  HandleVector<BlockId, Block> blocks_;
  HandleVector<ValueId, ValueInfo> values_;

  std::vector<ExprId> operandPool_;
  std::vector<BlockId> targetPool_;

  std::vector<std::string> names_;
  std::unordered_map<std::string, uint32_t> nameIndex_;
  std::vector<std::string> passNames_;
  std::unordered_map<std::string, PassId> passIndex_;

  std::unordered_map<uint64_t, BlockId> blocksByAddress_;
  std::unordered_map<OpId, std::string> notes_;
};

}  // namespace xdec::il
