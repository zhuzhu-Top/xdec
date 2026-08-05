#include "xdec/pass/registry.h"

#include <algorithm>
#include <unordered_map>

namespace xdec::pass {

Result<void> Registry::add(std::unique_ptr<Pass> pass) {
  if (pass == nullptr) {
    return err(DiagCode::Internal, "cannot register a null pass");
  }
  const PassInfo& info = pass->info();
  if (info.name.empty()) {
    return err(DiagCode::Internal, "cannot register a pass with an empty name");
  }
  if (info.produces < info.level) {
    return err(DiagCode::Internal,
               "pass '{}': produces ({}) is below its own level ({})", info.name,
               il::toString(info.produces), il::toString(info.level));
  }
  if (find(info.name) != nullptr) {
    return err(DiagCode::Internal, "duplicate pass name '{}'", info.name);
  }
  passes_.push_back(std::move(pass));
  return {};
}

Pass* Registry::find(std::string_view name) const noexcept {
  for (const auto& pass : passes_) {
    if (pass->info().name == name) {
      return pass.get();
    }
  }
  return nullptr;
}

std::vector<std::string_view> Registry::names() const {
  std::vector<std::string_view> out;
  out.reserve(passes_.size());
  for (const auto& pass : passes_) {
    out.push_back(pass->info().name);
  }
  return out;
}

Result<std::vector<Pass*>> Registry::resolve(il::Maturity from,
                                             il::Maturity target) const {
  if (from > target) {
    return err(DiagCode::Internal, "cannot resolve a pipeline from {} backwards to {}",
               il::toString(from), il::toString(target));
  }

  // Candidate set: the passes the walk can use. A pass whose level is behind
  // the starting maturity is history; a pass producing beyond the target is
  // out of scope for this pipeline.
  std::vector<Pass*> candidates;
  for (const auto& pass : passes_) {
    const PassInfo& info = pass->info();
    if (info.level >= from && info.produces <= target) {
      candidates.push_back(pass.get());
    }
  }

  // Requirement edges, validated while they are wired. A requirement that
  // names nothing registered is a typo; one that names a pass outside the
  // candidate set is either already satisfied (its level precedes `from`) or
  // a contradiction (it would produce beyond the target).
  std::unordered_map<std::string_view, std::vector<Pass*>> dependents;
  std::unordered_map<std::string_view, unsigned> pending;
  for (Pass* pass : candidates) {
    const PassInfo& info = pass->info();
    unsigned count = 0;
    for (const std::string& requirement : info.requirements) {
      Pass* required = find(requirement);
      if (required == nullptr) {
        return err(DiagCode::Internal, "pass '{}' requires '{}', which is not registered",
                   info.name, requirement);
      }
      if (required->info().level < from) {
        continue;  // ran before the walk started; satisfied by definition
      }
      if (required->info().produces > target) {
        return err(DiagCode::Internal,
                   "pass '{}' requires '{}', which produces {} — beyond the target {}",
                   info.name, requirement, il::toString(required->info().produces),
                   il::toString(target));
      }
      dependents[required->info().name].push_back(pass);
      ++count;
    }
    pending.emplace(info.name, count);
  }

  // Kahn's algorithm with a deterministic ready order: lowest level first,
  // then registration order. Registration order is the candidate index, which
  // is stable because candidates were collected in registration order.
  std::unordered_map<std::string_view, std::size_t> order;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    order.emplace(candidates[index]->info().name, index);
  }
  auto before = [&](const Pass* a, const Pass* b) {
    if (a->info().level != b->info().level) {
      return a->info().level < b->info().level;
    }
    // Same level: passes that stay at the level run before passes that advance
    // it. Advancing first would strand every same-level pass behind the level
    // wall it can never climb back over.
    if (a->info().produces != b->info().produces) {
      return a->info().produces < b->info().produces;
    }
    return order[a->info().name] < order[b->info().name];
  };
  // std::priority_queue adapts cleaner than a hand-maintained heap here.
  auto greater = [&](const Pass* a, const Pass* b) { return before(b, a); };

  std::vector<Pass*> ready;
  for (Pass* pass : candidates) {
    if (pending[pass->info().name] == 0) {
      ready.push_back(pass);
    }
  }
  std::make_heap(ready.begin(), ready.end(), greater);

  std::vector<Pass*> pipeline;
  pipeline.reserve(candidates.size());
  while (!ready.empty()) {
    std::pop_heap(ready.begin(), ready.end(), greater);
    Pass* next = ready.back();
    ready.pop_back();
    pipeline.push_back(next);
    for (Pass* dependent : dependents[next->info().name]) {
      unsigned& left = pending[dependent->info().name];
      if (--left == 0) {
        ready.push_back(dependent);
        std::push_heap(ready.begin(), ready.end(), greater);
      }
    }
  }

  if (pipeline.size() != candidates.size()) {
    std::string cycle;
    for (const Pass* pass : candidates) {
      if (pending[pass->info().name] > 0) {
        cycle += (cycle.empty() ? "" : ", ");
        cycle += pass->info().name;
      }
    }
    return err(DiagCode::Internal, "pass requirement cycle involving: {}", cycle);
  }

  // The walk itself: simulate the pipeline and confirm every pass actually
  // finds the function at its level. A hole here means the registry simply
  // has no pass that bridges one maturity to the next.
  il::Maturity at = from;
  for (const Pass* pass : pipeline) {
    const PassInfo& info = pass->info();
    if (info.level != at) {
      return err(DiagCode::Internal,
                 "gap in the maturity walk: pass '{}' needs the function at {}, but the "
                 "pipeline reaches only {} — no pass bridges {} to {}",
                 info.name, il::toString(info.level), il::toString(at), il::toString(at),
                 il::toString(info.level));
    }
    at = info.produces;
  }
  if (at != target) {
    return err(DiagCode::Internal, "pipeline from {} reaches only {}; no pass bridges {} to {}",
               il::toString(from), il::toString(at), il::toString(at), il::toString(target));
  }
  return pipeline;
}

}  // namespace xdec::pass
