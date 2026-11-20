#include "xdec/devirt/memory.h"

#include <algorithm>
#include <cstring>

namespace xdec::devirt {

const MemoryRegion* MemoryRegionMap::findByAddress(uint64_t va) const noexcept {
  for (const MemoryRegion& r : regions_) {
    if (r.fixedBase.has_value() && va >= *r.fixedBase &&
        va - *r.fixedBase < r.size) {
      return &r;
    }
  }
  return nullptr;
}

const MemoryRegion* MemoryRegionMap::findByBaseReg(il::RegId reg) const noexcept {
  if (!reg.valid()) {
    return nullptr;
  }
  for (const MemoryRegion& r : regions_) {
    if (r.baseReg.has_value() && *r.baseReg == reg) {
      return &r;
    }
  }
  return nullptr;
}

LayeredByteReader::LayeredByteReader() : state_(std::make_shared<State>()) {}

LayeredByteReader::LayeredByteReader(ByteReader base, MemoryFacts baseFacts)
    : state_(std::make_shared<State>()) {
  state_->base = std::move(base);
  state_->baseFacts = std::move(baseFacts);
}

void LayeredByteReader::setBase(ByteReader base, MemoryFacts baseFacts) {
  state_->base = std::move(base);
  state_->baseFacts = std::move(baseFacts);
}

void LayeredByteReader::addOverlay(uint64_t baseVa, std::vector<uint8_t> data,
                                   bool immutable) {
  state_->overlays.push_back(Overlay{baseVa, std::move(data), immutable});
}

Result<void> LayeredByteReader::read(uint64_t va, std::span<std::byte> out) const {
  if (out.empty()) {
    return {};
  }
  const std::size_t n = out.size();
  // Fast path: check newest overlay covering the entire span.
  for (auto it = state_->overlays.rbegin(); it != state_->overlays.rend(); ++it) {
    if (va >= it->baseVa && va - it->baseVa <= it->data.size() &&
        n <= it->data.size() - static_cast<std::size_t>(va - it->baseVa)) {
      const std::size_t off = static_cast<std::size_t>(va - it->baseVa);
      std::memcpy(out.data(), it->data.data() + off, n);
      return {};
    }
  }

  // Fallback: base read per byte/span, then patch any overlapping overlays in order.
  std::vector<uint8_t> filled(n, 0);
  if (state_->base) {
    if (auto baseRes = state_->base(va, out); baseRes) {
      std::fill(filled.begin(), filled.end(), 1);
    }
  }
  for (const Overlay& ov : state_->overlays) {
    if (ov.data.empty()) {
      continue;
    }
    const uint64_t ovEnd = ov.baseVa + ov.data.size();
    const uint64_t reqEnd = va + n;
    if (reqEnd <= ov.baseVa || va >= ovEnd) {
      continue;
    }
    const uint64_t lo = std::max(va, ov.baseVa);
    const uint64_t hi = std::min(reqEnd, ovEnd);
    const std::size_t dstOff = static_cast<std::size_t>(lo - va);
    const std::size_t srcOff = static_cast<std::size_t>(lo - ov.baseVa);
    const std::size_t len = static_cast<std::size_t>(hi - lo);
    const auto beginDiff = static_cast<std::ptrdiff_t>(dstOff);
    const auto endDiff = static_cast<std::ptrdiff_t>(dstOff + len);
    std::memcpy(out.data() + dstOff, ov.data.data() + srcOff, len);
    std::fill(filled.begin() + beginDiff, filled.begin() + endDiff, 1);
  }
  for (const uint8_t ok : filled) {
    if (ok == 0) {
      return err(DiagCode::UnmappedAddress, "unmapped read at {:#x} ({} bytes)", va, n);
    }
  }
  return {};
}

bool LayeredByteReader::isImmutable(uint64_t va, uint64_t size) const {
  if (size == 0) {
    return false;
  }
  for (auto it = state_->overlays.rbegin(); it != state_->overlays.rend(); ++it) {
    if (va >= it->baseVa && va - it->baseVa <= it->data.size() &&
        size <= it->data.size() - static_cast<uint64_t>(va - it->baseVa)) {
      return it->immutable;
    }
  }
  return state_->baseFacts.isImmutable(va, size);
}

ByteReader LayeredByteReader::asByteReader() const {
  auto state = state_;
  return [state](uint64_t va, std::span<std::byte> out) -> Result<void> {
    LayeredByteReader view;
    view.state_ = state;
    return view.read(va, out);
  };
}

MemoryFacts LayeredByteReader::asMemoryFacts() const {
  auto state = state_;
  MemoryFacts facts = state_->baseFacts;
  facts.immutable = [state](uint64_t va, uint64_t size) -> bool {
    LayeredByteReader view;
    view.state_ = state;
    return view.isImmutable(va, size);
  };
  return facts;
}

std::span<const LayeredByteReader::Overlay> LayeredByteReader::overlays() const noexcept {
  return state_->overlays;
}

}  // namespace xdec::devirt
