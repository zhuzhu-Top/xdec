// Layered memory views and explicit region alias contracts for devirtualization.
//
// Lets callers overlay decrypted/relocated payloads (e.g. an unpacked ELF/XIMG
// or runtime-initialized data pages) over a base image ByteReader, mark
// immutable ranges for constant folding, and declare private VM register/stack
// regions separately from external I/O memory.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xdec/il/op.h"
#include "xdec/support/reader.h"
#include "xdec/support/result.h"

namespace xdec::devirt {

enum class RegionKind : uint8_t {
  ImmutableData,   // Read-only bytecode or constant tables: safe for const-fold-memory
  PrivateRegFile,  // VM register backing buffer: no external aliasing
  PrivateStack,    // VM private stack buffer: eligible for stack slot promotion & DSE
  ExternalMemory,  // Caller input/output buffers: preserve strict memory semantics
};

struct MemoryRegion {
  std::string name;
  RegionKind kind = RegionKind::ExternalMemory;
  std::optional<uint64_t> fixedBase;
  std::optional<il::RegId> baseReg;
  uint64_t size = 0;
};

class MemoryRegionMap {
 public:
  void add(MemoryRegion region) { regions_.push_back(std::move(region)); }
  [[nodiscard]] std::span<const MemoryRegion> regions() const noexcept { return regions_; }
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }

  [[nodiscard]] const MemoryRegion* findByAddress(uint64_t va) const noexcept;
  [[nodiscard]] const MemoryRegion* findByBaseReg(il::RegId reg) const noexcept;

 private:
  std::vector<MemoryRegion> regions_;
};

class LayeredByteReader {
 public:
  struct Overlay {
    uint64_t baseVa = 0;
    std::vector<uint8_t> data;
    bool immutable = false;
  };

  LayeredByteReader();
  explicit LayeredByteReader(ByteReader base, MemoryFacts baseFacts = {});

  void setBase(ByteReader base, MemoryFacts baseFacts = {});
  void addOverlay(uint64_t baseVa, std::vector<uint8_t> data, bool immutable = true);

  [[nodiscard]] Result<void> read(uint64_t va, std::span<std::byte> out) const;
  [[nodiscard]] bool isImmutable(uint64_t va, uint64_t size) const;

  [[nodiscard]] ByteReader asByteReader() const;
  [[nodiscard]] MemoryFacts asMemoryFacts() const;
  [[nodiscard]] std::span<const Overlay> overlays() const noexcept;

 private:
  struct State {
    ByteReader base;
    MemoryFacts baseFacts;
    std::vector<Overlay> overlays;
  };
  std::shared_ptr<State> state_;
};

}  // namespace xdec::devirt
