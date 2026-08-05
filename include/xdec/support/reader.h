// A byte source over mapped memory, minimal by design.
//
// The lifter reads instruction bytes through this, and passes whose job
// needs memory (jump-table resolution reads the table through it) take the
// same shape, so a BinaryImage, a test's byte map, or a remote process all
// plug in without the consumer knowing the difference.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "xdec/support/result.h"

namespace xdec {

/// Reads `out.size()` bytes at `va`; fails where the range is not fully
/// mapped. Consumers treat failure as the edge of the knowable world, never
/// as zero-filled memory.
using ByteReader = std::function<Result<void>(uint64_t va, std::span<std::byte> out)>;

/// Whether every byte of `[va, va + size)` holds, for the whole life of the
/// program, exactly what the reader above returns for it: mapped, never
/// writable, and not patched by the loader. A consumer that gets `true` may
/// treat a read of that range as a constant of the program rather than as an
/// observation of memory.
using ImmutableMemoryTest = std::function<bool(uint64_t va, uint64_t size)>;

/// Whether `va` is in a region the program executes. What separates "this
/// constant is a function" from "this constant is readable".
using ExecutableMemoryTest = std::function<bool(uint64_t va)>;

/// What the loader writes into a slot before the program runs.
///
/// A file's bytes are not the whole story about what a pointer slot holds: a
/// relocated slot's bytes in the file are typically zero, and the value that
/// matters is the one the loader computes. Reading such a slot through a
/// ByteReader therefore answers a question nobody asked. This is how a consumer
/// asks the question it meant — which is often the only route to a function
/// pointer's target, since a relocated slot is by definition not immutable and
/// so may never be folded.
struct LoaderValue {
  /// An address inside this image.
  uint64_t address = 0;
  bool hasAddress = false;
  /// The name of a symbol defined in another module, when the slot is filled
  /// from one. There is no address to give — which module wins is a run-time
  /// question — but the name is the more useful half anyway.
  std::string importName;

  [[nodiscard]] bool known() const noexcept { return hasAddress || !importName.empty(); }
};

/// The loader's value for a slot, or an unknown one for an address no
/// relocation covers.
using LoaderValueQuery = std::function<LoaderValue(uint64_t va)>;

/// What a consumer may know about the address space beyond the bytes a
/// ByteReader hands over.
///
/// Reading is a capability every byte source has; these are claims only a real
/// image can make, and a test's byte map or a live process can hand over bytes
/// without being able to make them. So each one is optional, and the accessors
/// below answer "no" for an absent test rather than making the caller decide
/// what silence means: no information is never permission.
struct MemoryFacts {
  ImmutableMemoryTest immutable;
  ExecutableMemoryTest executable;
  LoaderValueQuery loader;

  [[nodiscard]] bool isImmutable(uint64_t va, uint64_t size) const {
    return immutable && immutable(va, size);
  }
  [[nodiscard]] bool isExecutable(uint64_t va) const {
    return executable && executable(va);
  }
  [[nodiscard]] LoaderValue loaderValueAt(uint64_t va) const {
    return loader ? loader(va) : LoaderValue{};
  }
};

}  // namespace xdec
