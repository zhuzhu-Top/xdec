// Recognising the two-phi-site relay a flattening dispatcher's handlers
// leave behind for whichever registers they keep alive across the state
// machine.
//
// A register a handler may or may not touch (an AAPCS64 argument register,
// typically) gets exactly the phi placement register SSA always gives a
// multiply-defined value: one phi at `shape.merge`, merging every handler's
// own exit value, and a second phi at `shape.hub`, merging the function's
// entry value against `merge`'s own (the loop's back edge). Nothing here is
// dispatcher-specific in the IL -- it is ordinary dominance-frontier
// placement -- but the *pattern* it produces once printed is: a case that
// never touches the register still gets a line copying `merge`'s phi result
// out of `hub`'s, once per case, because printing does not yet know the copy
// is the identity it structurally has to be. This is what lets it know.
#pragma once

#include <optional>
#include <vector>

#include "xdec/analysis/dispatcher_shape.h"
#include "xdec/il/function.h"

namespace xdec::analysis {

/// One register relayed through the dispatcher's merge/hub pair.
struct LiveRegisterSlot {
  il::RegId reg;
  /// The phi at `shape.hub` merging every path that reaches the loop header
  /// (the function's entry, and `shape.merge`'s own back edge).
  il::OpId livePhiAtHub;
  /// The phi at `shape.merge` merging every handler's own exit value for
  /// `reg`.
  il::OpId shadowPhiAtMerge;
};

struct LiveRegisterFrame {
  std::vector<LiveRegisterSlot> slots;
};

/// One slot per register carrying a phi at both `shape.hub` and
/// `shape.merge` (the "reg:xN" note ssa_construct.cpp leaves on every phi it
/// places is what finds them, directly, rather than re-deriving it from
/// variable naming order, which carries no such guarantee). Nullopt when no
/// register does -- this dispatcher's handlers do not keep anything alive
/// across it worth folding, and nothing here is worth guessing about instead.
[[nodiscard]] std::optional<LiveRegisterFrame> matchLiveRegisterFrame(
    const il::Function& function, const DispatcherShape& shape);

enum class HandlerExitKind : uint8_t {
  /// Every slot's value flows through the handler unchanged: the relay's
  /// per-case copy into `shape.merge` (and the shared copy back out of it)
  /// says nothing an emitted case body needs to repeat.
  Passthrough,
  /// Some slots flow through unchanged and some do not (a call result
  /// landing in one register, say): only the ones that changed are worth a
  /// line.
  Partial,
  /// The handler never reaches `shape.merge` at all -- it returns, or ends
  /// in its own nested dispatch -- so the frame says nothing about it.
  Return,
};

struct HandlerFrameExit {
  HandlerExitKind kind = HandlerExitKind::Return;
  /// Parallel to `frame.slots`, valid only where `kind` is Passthrough or
  /// Partial: whether that slot is the exact expression `frame`'s hub phi
  /// carried into this handler, i.e. the handler left it alone.
  std::vector<bool> unchanged;
};

/// Classifies how `handler` -- one of the dispatch block's own switch
/// targets -- leaves `frame` on its way out.
[[nodiscard]] HandlerFrameExit classifyHandlerExit(const il::Function& function,
                                                   il::BlockId handler,
                                                   const DispatcherShape& shape,
                                                   const LiveRegisterFrame& frame);

/// Parallel to `frame.slots`: whether *every* handler that reaches
/// `shape.merge` leaves that slot unchanged. Where true, nothing in the
/// function needs the relay for that register at all -- not one case's save,
/// not the shared restore -- because every path into `shape.merge` already
/// carries the exact value `shape.hub`'s own phi expects back out of it. Most
/// dispatchers have at least one handler that changes at least one register
/// (that is the whole reason the relay exists), so this is often all false;
/// it is not nothing, though, for the registers a given state machine simply
/// never repurposes.
[[nodiscard]] std::vector<bool> unanimousPassthroughSlots(const il::Function& function,
                                                          const DispatcherShape& shape,
                                                          const LiveRegisterFrame& frame);

}  // namespace xdec::analysis
