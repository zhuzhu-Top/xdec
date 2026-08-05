#include "xdec/support/diag.h"

#include <format>

namespace xdec {

std::string_view toString(DiagCode code) noexcept {
  switch (code) {
    case DiagCode::Ok:
      return "ok";
    case DiagCode::Internal:
      return "internal";
    case DiagCode::NotImplemented:
      return "not-implemented";
    case DiagCode::IoError:
      return "io-error";
    case DiagCode::BadFormat:
      return "bad-format";
    case DiagCode::UnsupportedFormat:
      return "unsupported-format";
    case DiagCode::UnsupportedArch:
      return "unsupported-arch";
    case DiagCode::OutOfRange:
      return "out-of-range";
    case DiagCode::UnmappedAddress:
      return "unmapped-address";
    case DiagCode::DecodeFailure:
      return "decode-failure";
    case DiagCode::LiftFailure:
      return "lift-failure";
    case DiagCode::ParseError:
      return "parse-error";
    case DiagCode::VerifyFailure:
      return "verify-failure";
    case DiagCode::AnalysisLimit:
      return "analysis-limit";
    case DiagCode::PluginError:
      return "plugin-error";
  }
  return "unknown";
}

std::string Diag::format() const {
  std::string text = std::format("{}: {}", toString(code_), message_);
  if (hasAddress()) {
    text += std::format(" (at 0x{:x})", address_);
  }
  for (const std::string& note : notes_) {
    text += std::format("\n  note: {}", note);
  }
  return text;
}

}  // namespace xdec
