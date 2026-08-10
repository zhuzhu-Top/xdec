// Category-based logging.
//
// Each subsystem declares a category; verbosity is set per category, because
// debugging a decompiler usually means turning one subsystem up to Trace while
// keeping everything else quiet. Configure via the XDEC_LOG environment
// variable, e.g. XDEC_LOG=binary=debug,lift=trace or XDEC_LOG=trace for all.
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/support/compiler.h"

namespace xdec {

enum class LogLevel : uint8_t {
  Off = 0,
  Error,
  Warn,
  Info,
  Debug,
  Trace,
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;
[[nodiscard]] bool parseLogLevel(std::string_view text, LogLevel& out) noexcept;

class LogCategory {
 public:
  /// Registers the category. Intended for use as a namespace-scope object.
  explicit LogCategory(std::string_view name) noexcept;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] LogLevel level() const noexcept { return level_; }
  void setLevel(LogLevel level) noexcept { level_ = level; }
  [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level <= level_; }

 private:
  std::string_view name_;
  LogLevel level_ = LogLevel::Warn;
};

/// Applies the XDEC_LOG environment variable. Called automatically on the
/// first log emission; call explicitly to control ordering.
void initLoggingFromEnvironment();

/// Sets the level of one registered category by name. Returns false if unknown.
bool setLogLevel(std::string_view categoryName, LogLevel level);

/// Sets the level of every registered category.
void setAllLogLevels(LogLevel level);

[[nodiscard]] std::vector<LogCategory*> logCategories();

namespace detail {
void emitLog(const LogCategory& category, LogLevel level, std::string_view message);
}  // namespace detail

template <class... Args>
void logMessage(const LogCategory& category, LogLevel level,
                std::format_string<Args...> fmt, Args&&... args) {
  detail::emitLog(category, level, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace xdec

/// Declares a category accessor in a header.
#define XDEC_DECLARE_LOG_CATEGORY(accessor) ::xdec::LogCategory& accessor() noexcept;

/// Defines a category in exactly one translation unit. The function-local
/// static avoids static initialisation order problems between subsystems.
#define XDEC_DEFINE_LOG_CATEGORY(accessor, nameLiteral) \
  ::xdec::LogCategory& accessor() noexcept {            \
    static ::xdec::LogCategory category{nameLiteral};   \
    return category;                                    \
  }

#define XDEC_LOG(categoryExpr, levelValue, ...)                          \
  do {                                                                   \
    ::xdec::LogCategory& xdecLogCat = (categoryExpr);                    \
    if (xdecLogCat.enabled(levelValue)) {                                \
      ::xdec::logMessage(xdecLogCat, levelValue, __VA_ARGS__);           \
    }                                                                    \
  } while (0)

#define XDEC_LOG_ERROR(cat, ...) XDEC_LOG(cat, ::xdec::LogLevel::Error, __VA_ARGS__)
#define XDEC_LOG_WARN(cat, ...) XDEC_LOG(cat, ::xdec::LogLevel::Warn, __VA_ARGS__)
#define XDEC_LOG_INFO(cat, ...) XDEC_LOG(cat, ::xdec::LogLevel::Info, __VA_ARGS__)
#define XDEC_LOG_DEBUG(cat, ...) XDEC_LOG(cat, ::xdec::LogLevel::Debug, __VA_ARGS__)
#define XDEC_LOG_TRACE(cat, ...) XDEC_LOG(cat, ::xdec::LogLevel::Trace, __VA_ARGS__)
