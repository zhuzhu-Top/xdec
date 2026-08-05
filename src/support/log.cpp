#include "xdec/support/log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace xdec {
namespace {

/// Parsed XDEC_LOG contents. Categories are constructed lazily on first use, so
/// the configuration has to be consulted at registration time rather than
/// applied in one up-front sweep.
struct EnvironmentConfig {
  std::optional<LogLevel> globalLevel;
  std::unordered_map<std::string, LogLevel> perCategory;
};

struct Registry {
  std::mutex mutex;
  std::vector<LogCategory*> categories;
  EnvironmentConfig config;
  bool configParsed = false;
};

Registry& registry() {
  static Registry instance;
  return instance;
}

void parseSpec(std::string_view spec, EnvironmentConfig& config) {
  std::string_view remaining = spec;
  while (!remaining.empty()) {
    const std::size_t comma = remaining.find(',');
    const std::string_view clause = remaining.substr(0, comma);
    if (!clause.empty()) {
      const std::size_t separator = clause.find('=');
      LogLevel level = LogLevel::Warn;
      if (separator == std::string_view::npos) {
        if (parseLogLevel(clause, level)) {
          config.globalLevel = level;
        }
      } else if (parseLogLevel(clause.substr(separator + 1), level)) {
        config.perCategory.emplace(std::string{clause.substr(0, separator)}, level);
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    remaining = remaining.substr(comma + 1);
  }
}

/// Caller must hold the registry mutex.
const EnvironmentConfig& environmentConfigLocked(Registry& reg) {
  if (!reg.configParsed) {
    reg.configParsed = true;
    if (const char* spec = std::getenv("XDEC_LOG"); spec != nullptr) {
      parseSpec(spec, reg.config);
    }
  }
  return reg.config;
}

}  // namespace

std::string_view toString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Off:
      return "off";
    case LogLevel::Error:
      return "error";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Info:
      return "info";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Trace:
      return "trace";
  }
  return "?";
}

bool parseLogLevel(std::string_view text, LogLevel& out) noexcept {
  if (text == "off" || text == "none") {
    out = LogLevel::Off;
  } else if (text == "error") {
    out = LogLevel::Error;
  } else if (text == "warn" || text == "warning") {
    out = LogLevel::Warn;
  } else if (text == "info") {
    out = LogLevel::Info;
  } else if (text == "debug") {
    out = LogLevel::Debug;
  } else if (text == "trace" || text == "all") {
    out = LogLevel::Trace;
  } else {
    return false;
  }
  return true;
}

LogCategory::LogCategory(std::string_view name) noexcept : name_(name) {
  Registry& reg = registry();
  const std::lock_guard<std::mutex> lock{reg.mutex};
  reg.categories.push_back(this);

  const EnvironmentConfig& config = environmentConfigLocked(reg);
  if (config.globalLevel.has_value()) {
    level_ = *config.globalLevel;
  }
  if (auto it = config.perCategory.find(std::string{name}); it != config.perCategory.end()) {
    level_ = it->second;
  }
}

void initLoggingFromEnvironment() {
  Registry& reg = registry();
  const std::lock_guard<std::mutex> lock{reg.mutex};
  const EnvironmentConfig& config = environmentConfigLocked(reg);
  for (LogCategory* category : reg.categories) {
    if (config.globalLevel.has_value()) {
      category->setLevel(*config.globalLevel);
    }
    const auto it = config.perCategory.find(std::string{category->name()});
    if (it != config.perCategory.end()) {
      category->setLevel(it->second);
    }
  }
}

bool setLogLevel(std::string_view categoryName, LogLevel level) {
  Registry& reg = registry();
  const std::lock_guard<std::mutex> lock{reg.mutex};
  bool found = false;
  for (LogCategory* category : reg.categories) {
    if (category->name() == categoryName) {
      category->setLevel(level);
      found = true;
    }
  }
  return found;
}

void setAllLogLevels(LogLevel level) {
  Registry& reg = registry();
  const std::lock_guard<std::mutex> lock{reg.mutex};
  for (LogCategory* category : reg.categories) {
    category->setLevel(level);
  }
}

std::vector<LogCategory*> logCategories() {
  Registry& reg = registry();
  const std::lock_guard<std::mutex> lock{reg.mutex};
  std::vector<LogCategory*> result = reg.categories;
  std::sort(result.begin(), result.end(), [](const LogCategory* a, const LogCategory* b) {
    return a->name() < b->name();
  });
  return result;
}

namespace detail {

void emitLog(const LogCategory& category, LogLevel level, std::string_view message) {
  static std::mutex outputMutex;
  const std::lock_guard<std::mutex> lock{outputMutex};
  const std::string line =
      std::format("[{:<5} {}] {}\n", toString(level), category.name(), message);
  std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace detail
}  // namespace xdec
