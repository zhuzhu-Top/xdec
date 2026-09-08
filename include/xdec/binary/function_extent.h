// Inferring where one function stops when the symbol table will not say.
//
// A sized symbol is a recorded extent. An unsized name (a dyld shared cache
// nlist, a label in the middle of a dispatcher) is only a name: using the
// next one along as this function's end cuts a flattened body into pieces
// whose live registers then look like unknown entry arguments. This walk
// skips those names and stops at the next thing that is actually a function.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "xdec/binary/image.h"

namespace xdec::binary {

/// True when `va` disassembles like a function start (a prologue). Absent,
/// only symbols that already carry a size count as function-like.
using FunctionStartTest = std::function<bool(uint64_t va)>;

/// Lowest defined symbol strictly above `address` that is a real function
/// start: it has a recorded size, or `isFunctionStart` says so. Unsized names
/// that fail the test (jump-table targets, flattened state blocks) are
/// skipped.
[[nodiscard]] std::optional<uint64_t> nextFunctionLikeAfter(
    const BinaryImage& image, uint64_t address,
    const FunctionStartTest& isFunctionStart = {});

/// Same walk over an already-materialised symbol list, for tests that have
/// no image.
[[nodiscard]] std::optional<uint64_t> nextFunctionLikeAfter(
    std::span<const Symbol> symbols, uint64_t address,
    const FunctionStartTest& isFunctionStart = {});

}  // namespace xdec::binary
