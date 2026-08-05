// Result<T>: a value or a Diag.
//
// Errors live behind a unique_ptr so that sizeof(Result<T>) stays close to
// sizeof(optional<T>) and the success path never allocates. That matters
// because Result appears on memory-read paths that run millions of times.
//
// Analysis failures use Result; broken internal invariants use XDEC_ASSERT.
// Exceptions are reserved for allocation failure.
#pragma once

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "xdec/support/compiler.h"
#include "xdec/support/diag.h"

namespace xdec {

/// The error half of a Result, implicitly convertible into any Result<T>.
class Unexpected {
 public:
  explicit Unexpected(Diag diag) : diag_(std::make_unique<Diag>(std::move(diag))) {}
  explicit Unexpected(std::unique_ptr<Diag> diag) noexcept : diag_(std::move(diag)) {}

  [[nodiscard]] std::unique_ptr<Diag> release() && noexcept { return std::move(diag_); }
  [[nodiscard]] const Diag& diag() const noexcept { return *diag_; }

 private:
  std::unique_ptr<Diag> diag_;
};

[[nodiscard]] inline Unexpected err(Diag diag) { return Unexpected{std::move(diag)}; }

template <class... Args>
[[nodiscard]] Unexpected err(DiagCode code, std::format_string<Args...> fmt, Args&&... args) {
  return Unexpected{Diag{code, std::format(fmt, std::forward<Args>(args)...)}};
}

/// Convenience overload for a message with no format arguments.
[[nodiscard]] inline Unexpected err(DiagCode code, std::string message) {
  return Unexpected{Diag{code, std::move(message)}};
}

template <class T>
class [[nodiscard]] Result {
 public:
  using ValueType = T;

  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Unexpected error)                       // NOLINT(google-explicit-constructor)
      : error_(std::move(error).release()) {}

  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  Result(Result&&) noexcept = default;
  Result& operator=(Result&&) noexcept = default;

  [[nodiscard]] bool hasValue() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return hasValue(); }

  [[nodiscard]] T& value() & {
    XDEC_ASSERT(hasValue(), "Result::value() on an error result");
    return *value_;
  }
  [[nodiscard]] const T& value() const& {
    XDEC_ASSERT(hasValue(), "Result::value() on an error result");
    return *value_;
  }
  [[nodiscard]] T&& value() && {
    XDEC_ASSERT(hasValue(), "Result::value() on an error result");
    return std::move(*value_);
  }

  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T&& operator*() && { return std::move(*this).value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }

  /// Returns `fallback` instead of aborting when this holds an error.
  [[nodiscard]] T valueOr(T fallback) const& {
    return hasValue() ? *value_ : std::move(fallback);
  }

  [[nodiscard]] const Diag& error() const {
    XDEC_ASSERT(!hasValue(), "Result::error() on a value result");
    return *error_;
  }

  /// Forwards the error into a Result of a different type. Used by XDEC_TRY.
  [[nodiscard]] Unexpected takeUnexpected() && {
    XDEC_ASSERT(!hasValue(), "takeUnexpected() on a value result");
    return Unexpected{std::move(error_)};
  }

 private:
  std::optional<T> value_;
  std::unique_ptr<Diag> error_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  using ValueType = void;

  Result() = default;
  Result(Unexpected error)  // NOLINT(google-explicit-constructor)
      : error_(std::move(error).release()) {}

  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  Result(Result&&) noexcept = default;
  Result& operator=(Result&&) noexcept = default;

  [[nodiscard]] bool hasValue() const noexcept { return error_ == nullptr; }
  explicit operator bool() const noexcept { return hasValue(); }

  void value() const {
    XDEC_ASSERT(hasValue(), "Result<void>::value() on an error result");
  }

  [[nodiscard]] const Diag& error() const {
    XDEC_ASSERT(!hasValue(), "Result::error() on a value result");
    return *error_;
  }

  [[nodiscard]] Unexpected takeUnexpected() && {
    XDEC_ASSERT(!hasValue(), "takeUnexpected() on a value result");
    return Unexpected{std::move(error_)};
  }

 private:
  std::unique_ptr<Diag> error_;
};

/// Spelling for a successful Result<void>.
[[nodiscard]] inline Result<void> ok() { return Result<void>{}; }

}  // namespace xdec

#define XDEC_DETAIL_CONCAT_(a, b) a##b
#define XDEC_DETAIL_CONCAT(a, b) XDEC_DETAIL_CONCAT_(a, b)
#define XDEC_DETAIL_TRY_VAR XDEC_DETAIL_CONCAT(xdecTryResult_, __LINE__)

/// Unwraps a Result into a new declaration, propagating the error on failure.
/// Usage: `XDEC_TRY(auto bytes, image.read(va, 8));`
///
/// Declares a variable in the enclosing scope, so it cannot be used twice on
/// one source line.
#define XDEC_TRY(decl, expr)                                 \
  auto XDEC_DETAIL_TRY_VAR = (expr);                         \
  if (!XDEC_DETAIL_TRY_VAR) [[unlikely]] {                   \
    return std::move(XDEC_DETAIL_TRY_VAR).takeUnexpected();  \
  }                                                          \
  decl = std::move(XDEC_DETAIL_TRY_VAR).value()

/// Propagates the error of a Result whose value is unused.
#define XDEC_TRY_VOID(expr)                                    \
  do {                                                         \
    auto XDEC_DETAIL_TRY_VAR = (expr);                         \
    if (!XDEC_DETAIL_TRY_VAR) [[unlikely]] {                   \
      return std::move(XDEC_DETAIL_TRY_VAR).takeUnexpected();  \
    }                                                          \
  } while (0)
