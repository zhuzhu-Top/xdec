// Structured diagnostics.
//
// The project rule is fail-loud: an analysis that cannot establish a fact
// reports a Diag rather than guessing. A decompiler that guesses produces
// plausible-looking wrong output, which is worse than no output.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xdec {

enum class DiagCode : uint16_t {
  Ok = 0,
  /// A broken internal invariant that input data should not be able to cause.
  Internal,
  /// A deliberate, known gap. Never used to paper over a wrong result.
  NotImplemented,
  IoError,
  BadFormat,
  UnsupportedFormat,
  UnsupportedArch,
  OutOfRange,
  /// A virtual address outside every mapped range of the image.
  UnmappedAddress,
  DecodeFailure,
  LiftFailure,
  ParseError,
  /// An IL invariant violation detected by the verifier.
  VerifyFailure,
  /// A budget (time, node count, iteration count) was exhausted.
  AnalysisLimit,
  PluginError,
};

[[nodiscard]] std::string_view toString(DiagCode code) noexcept;

/// Sentinel for "this diagnostic has no associated address".
inline constexpr uint64_t kNoAddress = ~uint64_t{0};

class Diag {
 public:
  Diag() = default;
  Diag(DiagCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] DiagCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] uint64_t address() const noexcept { return address_; }
  [[nodiscard]] bool hasAddress() const noexcept { return address_ != kNoAddress; }
  [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }

  /// Attaches the virtual address the failure relates to.
  Diag&& at(uint64_t address) && {
    address_ = address;
    return std::move(*this);
  }
  Diag& at(uint64_t address) & {
    address_ = address;
    return *this;
  }

  /// Adds a contextual note as the error propagates outward.
  Diag&& note(std::string text) && {
    notes_.push_back(std::move(text));
    return std::move(*this);
  }
  Diag& note(std::string text) & {
    notes_.push_back(std::move(text));
    return *this;
  }

  /// Renders as `code: message [at 0x...]` followed by indented notes.
  [[nodiscard]] std::string format() const;

 private:
  DiagCode code_ = DiagCode::Internal;
  std::string message_;
  uint64_t address_ = kNoAddress;
  std::vector<std::string> notes_;
};

}  // namespace xdec
