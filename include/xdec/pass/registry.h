// Pass registration and pipeline resolution.
//
// Passes register by name; pipelines are resolved, never hand-written. The
// resolver answers "to take a function from maturity A to maturity B, which
// passes run, in what order" by combining three orderings:
//
//   1. Level order — a pass can only run at its declared level, so the
//      maturity walk itself imposes a backbone.
//   2. Requirement edges — `requirements` names a pass that must precede it.
//   3. Registration order — the deterministic tie-break when neither of the
//      above constrains two passes, so a pipeline never depends on container
//      iteration order.
//
// Everything the resolver cannot satisfy is a configuration error reported
// with names: an unknown requirement, a requirement cycle, or a gap in the
// level walk (no pass carries the function from one maturity to the next).
// Scheduling bugs surface here, once, instead of as misordered output.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/il/maturity.h"
#include "xdec/pass/pass.h"
#include "xdec/support/result.h"

namespace xdec::pass {

class Registry {
 public:
  /// Takes ownership. A duplicate name is an error: names carry both
  /// scheduling and provenance, and two passes answering to one name breaks
  /// both silently.
  [[nodiscard]] Result<void> add(std::unique_ptr<Pass> pass);

  /// Non-owning handle to a registered pass. Non-const even from a const
  /// registry: running a pass is the whole point, and run() is not const.
  [[nodiscard]] Pass* find(std::string_view name) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return passes_.size(); }
  [[nodiscard]] std::vector<std::string_view> names() const;

  /// The ordered pipeline that walks a function from `from` to `target`.
  ///
  /// A registered pass joins the pipeline when its level is reachable on the
  /// walk (level >= from, produces <= target); requirements satisfied by the
  /// function's starting maturity (the required pass's level is behind us)
  /// are taken as already met, and requirements pointing beyond the target
  /// are configuration errors. The result is deterministic: Kahn's algorithm
  /// over requirement edges, breaking ties by level and then registration
  /// order.
  [[nodiscard]] Result<std::vector<Pass*>> resolve(il::Maturity from,
                                                   il::Maturity target) const;

 private:
  std::vector<std::unique_ptr<Pass>> passes_;
};

}  // namespace xdec::pass
