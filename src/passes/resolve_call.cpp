// makeResolveCallPass: proves an indirect call's target, or says what it is
// (see the header for why those are the only two outcomes).
#include "xdec/passes/resolve_call.h"

#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xdec/analysis/call_target.h"
#include "xdec/analysis/image_eval.h"
#include "xdec/binary/cache_pointer.h"
#include "xdec/il/function.h"

namespace xdec::passes {

namespace {

/// A reader that serves only what can never change.
///
/// This is the whole soundness argument of the rewrite below. ImageEval reads
/// memory through whatever reader it is given, and the plain image reader will
/// happily hand over the current contents of `.data` — fine for enumerating a
/// jump table, where every entry the loader might install is still an address
/// inside this function and a wrong guess fails loudly at verification, but not
/// fine for turning a call into `sub_1234()`. A function pointer in writable
/// memory is whatever ran before this code ran; the bytes in the file are its
/// initial value at best and padding at worst.
///
/// So the evaluator gets a memory in which the mutable simply does not exist.
/// Unreadable is already a value ImageEval handles — it yields top, and a top
/// target does not resolve — which means restricting the reader needs no new
/// concept anywhere: it makes the answer "unknown" exactly where the honest
/// answer is "unknown".
[[nodiscard]] ByteReader immutableOnly(const ByteReader& image, const MemoryFacts& facts) {
  return [&image, &facts](uint64_t va, std::span<std::byte> out) -> Result<void> {
    if (!facts.isImmutable(va, out.size())) {
      return err(DiagCode::Internal, "{:#x} is not immutable memory", va);
    }
    return image(va, out);
  };
}

/// If `va` is not executable but its shared-cache-decoded form is, rewrites
/// `va` to the decoded address and reports true. `va` is left untouched
/// (returns false) when it is already executable, or when decoding does not
/// help -- see resolve_indirect.cpp's own table-entry version of this same
/// fallback for the full rationale (a slot's raw bytes may carry a tag no
/// table-index arithmetic was involved to strip).
[[nodiscard]] bool decodeIfNeeded(const MemoryFacts& facts, uint64_t& va) {
  if (facts.isExecutable(va)) {
    return true;
  }
  static constexpr binary::CachePointerDecoder kCachePointer;
  const uint64_t decoded = kCachePointer.decode(va);
  if (decoded == va || !facts.isExecutable(decoded)) {
    return false;
  }
  va = decoded;
  return true;
}

class ResolveCall final : public pass::FunctionPass {
 public:
  ResolveCall()
      : FunctionPass([] {
          pass::PassInfo info;
          info.name = "resolve-call";
          info.level = il::Maturity::Ssa;
          // Ssa, not Resolved: this pass says nothing about the CFG, and
          // resolve-indirect is what earns the level crossing.
          info.produces = il::Maturity::Ssa;
          info.requirements = {"ssa-construct"};
          return info;
        }()) {}

  Result<bool> run(pass::Context& context) override {
    il::Function& function = context.function();
    const ByteReader* image = context.image();
    const MemoryFacts& facts = context.memoryFacts();

    // Without an image there is nothing to prove a target from, but the shapes
    // are still readable off the IL, so the pass still has something to say.
    // One evaluator for the whole function: its memo is what keeps a dispatcher
    // whose calls all share one address computation from being re-walked once
    // per call.
    std::optional<analysis::ImageEval> eval;
    if (image != nullptr) {
      // entryRegFacts, not just the image: a platform-leaked EntryReg
      // matters here exactly as much as it does to resolve-indirect -- a
      // call through a pointer derived from one is the same shape as an
      // indirect branch through it, just spelled as a call at the source
      // level.
      eval.emplace(function, immutableOnly(*image, facts), context.entryRegFacts());
    }

    bool changed = false;
    for (const il::BlockId blockId : function.blockHandles()) {
      for (const il::OpId opId : function.block(blockId).ops) {
        const il::Op& op = function.op(opId);
        if (op.code != il::OpCode::Call) {
          continue;
        }
        const auto operands = function.operands(op);
        if (operands.empty()) {
          continue;
        }
        uint64_t direct = 0;
        if (function.asConstant(operands[0], direct)) {
          // Already a constant -- but not necessarily one this pass (or
          // whichever upstream fold produced it, e.g. const-fold-memory
          // reading a pointer slot verbatim) ever checked is a sane branch
          // target: a dyld shared cache pointer slot's raw bytes may still
          // carry a tag (see decodeIfNeeded). Untagged and executable is the
          // overwhelmingly common case and costs one cheap check; a target
          // that needs decoding is rewritten in place so every later pass
          // (and the emitted C) sees the real address, not the tag.
          if (uint64_t decoded = direct; decodeIfNeeded(facts, decoded) && decoded != direct) {
            std::vector<il::ExprId> rewritten(operands.begin(), operands.end());
            rewritten[0] = function.constant(function.expr(operands[0]).type, decoded);
            function.setOperands(opId, rewritten);
            changed = true;
          }
          continue;  // already a direct call; nothing further to prove
        }
        changed |= handle(function, eval, facts, opId);
      }
    }
    return changed;
  }

 private:
  /// One call: prove it, or describe it. Returns whether the IL changed, which
  /// a note does not count as — notes carry no meaning for any other pass, so
  /// reporting one as a change would spin a fixpoint loop over nothing.
  static bool handle(il::Function& function, std::optional<analysis::ImageEval>& eval,
                     const MemoryFacts& facts, il::OpId opId) {
    const auto operands = function.operands(function.op(opId));
    const std::vector<il::ExprId> arguments(operands.begin(), operands.end());
    const il::ExprId target = arguments[0];

    uint64_t resolved = 0;
    if (eval.has_value() && converges(*eval, facts, target, resolved)) {
      std::vector<il::ExprId> rewritten = arguments;
      rewritten[0] = function.constant(function.expr(target).type, resolved);
      function.setOperands(opId, rewritten);
      // The call now prints as a direct one, which loses the fact that the
      // binary did not spell it that way — and for a reader trying to tell
      // hand-written code from an obfuscator's dispatch, that is the
      // interesting part.
      describe(function, opId, "call target proved constant from read-only memory");
      return true;
    }

    const analysis::CallTargetShape shape = analysis::describeCallTarget(function, target);
    describe(function, opId, analysis::describeShape(shape, slotFacts(facts, shape)));
    return false;
  }

  /// Adds this pass's account of the target to whatever an earlier pass already
  /// said about the call, rather than over it: a call an earlier pass created
  /// out of something else (recover-tailcall turns a branch into one) knows a
  /// thing about it that this pass cannot see, and replacing the note would
  /// throw it away. Written once even when the fixpoint scheduler runs this pass
  /// again, since a note is not a change and re-running must not accumulate one.
  static void describe(il::Function& function, il::OpId opId, std::string note) {
    const std::string_view existing = function.noteOn(opId);
    if (existing.find(note) != std::string_view::npos) {
      return;
    }
    function.appendNote(opId, note);
  }

  /// What the image knows about the slot a single-slot target reads.
  ///
  /// The loader value is the answer for the commonest unresolvable call there
  /// is: a pointer in writable memory whose bytes in the file are zero, filled
  /// in at load time. Reading it is useless and folding it is unsound — the slot
  /// is writable, so nothing rules out the program replacing the pointer — but
  /// the relocation states what goes in it, and for code that never replaces it
  /// that is the target.
  [[nodiscard]] static analysis::TargetSlotFacts slotFacts(
      const MemoryFacts& facts, const analysis::CallTargetShape& shape) {
    analysis::TargetSlotFacts slot;
    if (!shape.hasTableBase) {
      return slot;
    }
    slot.immutable = facts.isImmutable(shape.tableBase, 8);
    slot.loader = facts.loaderValueAt(shape.tableBase);
    return slot;
  }

  /// Whether `target` can only be one address, and that address is code.
  ///
  /// Both halves are load-bearing. Without the singleton test there is no
  /// target, only candidates. Without the executable test a converged value is
  /// just a number: a pointer table that happens to hold a string address would
  /// otherwise become a call to the middle of `.rodata`, which is a confident
  /// lie where "unresolved" was merely unhelpful. A pipeline that wires no
  /// executability facts resolves nothing, by the same rule.
  [[nodiscard]] static bool converges(analysis::ImageEval& eval, const MemoryFacts& facts,
                                      il::ExprId target, uint64_t& out) {
    const analysis::ValueSet set = eval.eval(target);
    if (set.isTop() || set.values().size() != 1) {
      return false;
    }
    uint64_t va = set.values()[0];
    if (!decodeIfNeeded(facts, va)) {
      return false;
    }
    out = va;
    return true;
  }
};

}  // namespace

std::unique_ptr<pass::Pass> makeResolveCallPass() {
  return std::make_unique<ResolveCall>();
}

}  // namespace xdec::passes
