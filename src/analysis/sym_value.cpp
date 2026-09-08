// SymValueSet (see the header).
#include "xdec/analysis/sym_value.h"

namespace xdec::analysis {

void SymValueSet::insert(uint64_t value) {
  if (top_) {
    return;
  }
  for (const uint64_t existing : values_) {
    if (existing == value) {
      return;
    }
  }
  if (values_.size() >= kCap) {
    top_ = true;
    values_.clear();
    return;
  }
  values_.push_back(value);
}

void SymValueSet::unite(const SymValueSet& other) {
  if (top_) {
    return;
  }
  if (other.top_) {
    top_ = true;
    values_.clear();
    return;
  }
  for (const uint64_t value : other.values_) {
    insert(value);
  }
}

}  // namespace xdec::analysis
