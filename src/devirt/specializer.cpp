#include "xdec/devirt/specializer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <queue>
#include <unordered_set>

#include "xdec/il/ceval.h"
#include "xdec/il/verify.h"
#include "xdec/support/bits.h"

namespace xdec::devirt {

namespace {

[[nodiscard]] std::string computeKey(const SpecializerOptions& opts, uint64_t pc,
                                     const SpecializerState& state) {
  if (opts.stateKeyFn) {
    return opts.stateKeyFn(pc, state);
  }
  if (opts.stateRegs.empty()) {
    return {};
  }
  std::string key;
  for (const il::RegId r : opts.stateRegs) {
    if (const auto v = state.get(r)) {
      key += std::format("{}:{:x};", r.index(), *v);
    } else {
      key += std::format("{}:?;", r.index());
    }
  }
  return key;
}

struct NodeKey {
  uint32_t blockIdx = 0;
  std::string stateKey;

  friend bool operator==(const NodeKey&, const NodeKey&) = default;
};

struct NodeKeyHash {
  std::size_t operator()(const NodeKey& k) const noexcept {
    std::size_t h = std::hash<uint32_t>{}(k.blockIdx);
    h ^= std::hash<std::string>{}(k.stateKey) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

/// Rebuilds an expression in `outFn`, substituting concrete register/value
/// constants where known and preserving symbolic expressions otherwise, then
/// runs `tryEvalConst` to collapse pure constant subtrees.
il::ExprId cloneAndFoldExpr(
    const il::Function& inFn, il::Function& outFn, il::ExprId id,
    const std::unordered_map<uint32_t, il::ExprId>& valueMap,
    std::unordered_map<uint32_t, il::ExprId>& memo) {
  if (!id.valid()) {
    return {};
  }
  if (const auto it = memo.find(id.index()); it != memo.end()) {
    return it->second;
  }

  const il::Expr& e = inFn.expr(id);
  il::ExprId result;
  switch (e.op) {
    case il::ExprOp::Const:
      result = outFn.constant(e.type, e.immediate);
      break;
    case il::ExprOp::Undef:
      result = outFn.undefined(e.type);
      break;
    case il::ExprOp::EntryReg:
      result = outFn.entryReg(il::RegId{static_cast<uint32_t>(e.immediate)});
      break;
    case il::ExprOp::Value: {
      const auto vit = valueMap.find(static_cast<uint32_t>(e.immediate));
      if (vit != valueMap.end()) {
        result = vit->second;
      } else {
        result = outFn.undefined(e.type);
      }
      break;
    }
    default: {
      il::Expr rebuilt = e;
      for (unsigned i = 0; i < e.operandCount; ++i) {
        rebuilt.operands[i] =
            cloneAndFoldExpr(inFn, outFn, e.operands[i], valueMap, memo);
      }
      result = outFn.intern(rebuilt);
      il::ConcreteValue folded{};
      if ((e.type.isScalarInteger() || e.type.isFlags()) &&
          il::tryEvalConst(outFn, result, folded)) {
        result = outFn.constant(e.type, folded.lo);
      }
      break;
    }
  }
  memo.emplace(id.index(), result);
  return result;
}

void writeRegWithViews(const il::RegisterFile& rf, il::RegId reg, uint64_t val,
                       SpecializerState& state) {
  const il::RegisterInfo& info = rf[reg];
  if (info.regClass == il::RegClass::Zero) {
    return;
  }
  const il::RegId root = rf.rootOf(reg);
  if (!info.isSubRegister()) {
    state.set(root, val);
    return;
  }
  if (info.zeroExtendsParent) {
    state.set(root, zeroExtend(val, info.bits));
    return;
  }
  if (const auto prev = state.get(root)) {
    const uint64_t m = info.bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << info.bits) - 1);
    const uint64_t updated =
        (*prev & ~(m << info.offsetInParent)) | ((val & m) << info.offsetInParent);
    state.set(root, updated);
  } else {
    state.clear(root);
  }
}

[[nodiscard]] std::optional<uint64_t> readRegWithViews(const il::RegisterFile& rf,
                                                       il::RegId reg,
                                                       const SpecializerState& state) {
  const il::RegisterInfo& info = rf[reg];
  if (info.regClass == il::RegClass::Zero) {
    return 0;
  }
  const il::RegId root = rf.rootOf(reg);
  const auto base = state.get(root);
  if (!base.has_value()) {
    return std::nullopt;
  }
  if (!info.isSubRegister()) {
    return *base;
  }
  return zeroExtend(*base >> info.offsetInParent, info.bits);
}

}  // namespace

Result<std::unique_ptr<il::Function>> Specializer::run(const il::Function& input,
                                                       const SpecializerOptions& options) {
  if (!input.entryBlock().valid()) {
    return err(DiagCode::Internal, "Specializer: input function has no entry block");
  }
  if (input.maturity() > il::Maturity::Cfg) {
    return err(DiagCode::Internal,
               "Specializer expects input function at Lifted or Cfg maturity");
  }

  auto regCopy = std::make_shared<const il::RegisterFile>(input.registers());
  auto outFn = std::make_unique<il::Function>(input.arch(), regCopy, input.entryVa());
  outFn->setName(input.name());

  const il::RegisterFile& rf = input.registers();
  std::unordered_map<NodeKey, il::BlockId, NodeKeyHash> outBlocks;
  std::queue<std::pair<NodeKey, SpecializerState>> worklist;

  // Allocate a distinct synthetic VA per specialized block so blockAt() remains
  // unambiguous across multiple specializations of the same native block.
  uint64_t nextSynthVa = 0x10000000ULL;
  const auto getOrCreateOutBlock = [&](const NodeKey& key,
                                       const SpecializerState& st,
                                       bool isEntry = false) -> il::BlockId {
    if (const auto it = outBlocks.find(key); it != outBlocks.end()) {
      return it->second;
    }
    const uint64_t va = isEntry ? input.block(il::BlockId{key.blockIdx}).va
                                : nextSynthVa;
    if (!isEntry) {
      nextSynthVa += 0x10;
    }
    const il::BlockId b = outFn->createBlock(va);
    if (isEntry) {
      outFn->setEntryBlock(b);
    }
    outBlocks.emplace(key, b);
    worklist.emplace(key, st);
    return b;
  };

  const NodeKey entryKey{
      input.entryBlock().index(),
      computeKey(options, input.block(input.entryBlock()).va, options.initialState)};
  (void)getOrCreateOutBlock(entryKey, options.initialState, /*isEntry=*/true);

  std::size_t visitedCount = 0;
  while (!worklist.empty()) {
    if (++visitedCount > options.maxNodes) {
      return err(DiagCode::AnalysisLimit,
                 "Specializer exceeded maxNodes ({})", options.maxNodes);
    }
    auto [curKey, state] = std::move(worklist.front());
    worklist.pop();

    const il::BlockId srcId{curKey.blockIdx};
    const il::Block& srcBlock = input.block(srcId);
    const il::BlockId outBlock = outBlocks.at(curKey);

    std::unordered_map<uint32_t, il::ExprId> valueMap;
    std::unordered_map<uint32_t, il::ExprId> exprMemo;
    // Emit concrete entry state writes only if needed? No — only symbolic ops
    // need ReadReg/WriteReg; known constants fold directly into expressions!

    bool terminated = false;
    for (const il::OpId oid : srcBlock.ops) {
      const il::Op& op = input.op(oid);
      const auto operands = input.operands(op);
      switch (op.code) {
        case il::OpCode::Nop:
          break;
        case il::OpCode::ReadReg: {
          if (const auto c = readRegWithViews(rf, op.reg(), state)) {
            valueMap[op.result.index()] = outFn->constant(op.type, *c);
          } else {
            const il::ValueId v = outFn->appendReadReg(outBlock, op.va, op.reg());
            valueMap[op.result.index()] = outFn->valueRef(v);
          }
          break;
        }
        case il::OpCode::WriteReg: {
          const il::ExprId val =
              cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
          uint64_t c = 0;
          if (outFn->asConstant(val, c)) {
            writeRegWithViews(rf, op.reg(), c, state);
            // Keep WriteReg only if `op.reg()` is NOT purely a state register,
            // OR omit state register writes when specialized away? Wait: a
            // return register (x0) written as a constant BEFORE `ret` must
            // still be visible! Keep WriteReg unless `op.reg()` is in stateRegs.
            const il::RegId root = rf.rootOf(op.reg());
            const bool isStateReg =
                std::find(options.stateRegs.begin(), options.stateRegs.end(), root) !=
                options.stateRegs.end();
            if (!isStateReg) {
              outFn->appendWriteReg(outBlock, op.va, op.reg(), val);
            }
          } else {
            state.clear(rf.rootOf(op.reg()));
            outFn->appendWriteReg(outBlock, op.va, op.reg(), val);
          }
          break;
        }
        case il::OpCode::Load: {
          const il::ExprId addr =
              cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
          uint64_t va = 0;
          const unsigned bytes = op.type.bits() / 8;
          if (options.memory != nullptr && bytes >= 1 && bytes <= 8 &&
              outFn->asConstant(addr, va) && options.memory->isImmutable(va, bytes)) {
            uint64_t loaded = 0;
            if (options.memory
                    ->read(va, std::span<std::byte>(
                                   reinterpret_cast<std::byte*>(&loaded), bytes))
                    .hasValue()) {
              valueMap[op.result.index()] = outFn->constant(op.type, loaded);
              break;
            }
          }
          const il::ValueId v = outFn->appendLoad(outBlock, op.va, op.type, addr);
          valueMap[op.result.index()] = outFn->valueRef(v);
          break;
        }
        case il::OpCode::Store: {
          const il::ExprId addr =
              cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
          const il::ExprId val =
              cloneAndFoldExpr(input, *outFn, operands[1], valueMap, exprMemo);
          outFn->appendStore(outBlock, op.va, op.type, addr, val);
          break;
        }
        case il::OpCode::Call: {
          std::vector<il::ExprId> args;
          args.reserve(operands.size());
          for (const il::ExprId a : operands) {
            args.push_back(cloneAndFoldExpr(input, *outFn, a, valueMap, exprMemo));
          }
          const il::OpId newCall = outFn->appendCall(outBlock, op.va, args[0], op.type);
          if (args.size() > 1) {
            outFn->setOperands(newCall, args);
          }
          if (op.result.valid()) {
            valueMap[op.result.index()] = outFn->valueRef(outFn->op(newCall).result);
          }
          // Conservative: non-state caller-saved regs become unknown across calls,
          // while stateRegs stay intact unless overwritten.
          break;
        }
        case il::OpCode::Intrinsic: {
          std::vector<il::ExprId> args;
          args.reserve(operands.size());
          for (const il::ExprId a : operands) {
            args.push_back(cloneAndFoldExpr(input, *outFn, a, valueMap, exprMemo));
          }
          const il::OpId intrinOp = outFn->appendIntrinsic(
              outBlock, op.va, input.nameOf(op.payload), op.type, args);
          if (op.result.valid() && outFn->op(intrinOp).result.valid()) {
            valueMap[op.result.index()] = outFn->valueRef(outFn->op(intrinOp).result);
          }
          break;
        }
        case il::OpCode::Branch: {
          const il::BlockId tgt = input.targets(op)[0];
          const NodeKey nextKey{tgt.index(),
                                computeKey(options, input.block(tgt).va, state)};
          const il::BlockId nextOut = getOrCreateOutBlock(nextKey, state);
          outFn->appendBranch(outBlock, op.va, nextOut);
          terminated = true;
          break;
        }
        case il::OpCode::CondBranch: {
          const il::ExprId cond =
              cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
          const auto tgts = input.targets(op);
          uint64_t c = 0;
          if (outFn->asConstant(cond, c)) {
            const il::BlockId chosen = (c & 1) != 0 ? tgts[0] : tgts[1];
            const NodeKey nextKey{chosen.index(),
                                  computeKey(options, input.block(chosen).va, state)};
            const il::BlockId nextOut = getOrCreateOutBlock(nextKey, state);
            outFn->appendBranch(outBlock, op.va, nextOut);
          } else {
            const NodeKey tKey{tgts[0].index(),
                               computeKey(options, input.block(tgts[0]).va, state)};
            const NodeKey fKey{tgts[1].index(),
                               computeKey(options, input.block(tgts[1]).va, state)};
            const il::BlockId tOut = getOrCreateOutBlock(tKey, state);
            const il::BlockId fOut = getOrCreateOutBlock(fKey, state);
            outFn->appendCondBranch(outBlock, op.va, cond, tOut, fOut);
          }
          terminated = true;
          break;
        }
        case il::OpCode::IndirectBranch: {
          const il::ExprId targetExpr =
              cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
          uint64_t targetVa = 0;
          if (outFn->asConstant(targetExpr, targetVa)) {
            const il::BlockId tgt = input.blockAt(targetVa);
            if (tgt.valid()) {
              const NodeKey nextKey{tgt.index(), computeKey(options, targetVa, state)};
              const il::BlockId nextOut = getOrCreateOutBlock(nextKey, state);
              outFn->appendBranch(outBlock, op.va, nextOut);
              terminated = true;
              break;
            }
          }
          std::vector<il::BlockId> outTgts;
          for (const il::BlockId t : input.targets(op)) {
            const NodeKey nextKey{t.index(),
                                  computeKey(options, input.block(t).va, state)};
            outTgts.push_back(getOrCreateOutBlock(nextKey, state));
          }
          const il::OpId br = outFn->appendIndirectBranch(outBlock, op.va, targetExpr);
          if (!outTgts.empty()) {
            outFn->setTargets(br, outTgts);
          }
          terminated = true;
          break;
        }
        case il::OpCode::Return: {
          const il::OpId ret = outFn->appendReturn(outBlock, op.va);
          if (!operands.empty()) {
            const il::ExprId v =
                cloneAndFoldExpr(input, *outFn, operands[0], valueMap, exprMemo);
            const il::ExprId retOps[] = {v};
            outFn->setOperands(ret, retOps);
          }
          terminated = true;
          break;
        }
        case il::OpCode::Unreachable:
          outFn->appendUnreachable(outBlock, op.va);
          terminated = true;
          break;
        case il::OpCode::Unimplemented:
          outFn->appendUnimplemented(outBlock, op.va, input.nameOf(op.payload));
          terminated = true;
          break;
        case il::OpCode::Phi:
        case il::OpCode::Count:
          break;
      }
      if (terminated) {
        break;
      }
    }
  }

  outFn->rebuildEdges();
  outFn->setMaturity(il::Maturity::Cfg);
  const il::VerifyReport report = il::verify(*outFn, il::Maturity::Cfg);
  if (!report.ok()) {
    return err(DiagCode::VerifyFailure,
               "Specializer produced invalid IL:\n{}", report.format());
  }
  return outFn;
}

}  // namespace xdec::devirt
