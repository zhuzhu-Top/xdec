// Everything about one `xdec` invocation that is not a command: logging,
// tracing, progress, deadlines, the watchdog, crash handling, and the run
// report. main() builds one, lets it take its options out of argv (they may
// appear anywhere on the command line), runs the command under its job, and
// finishes it.
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xdec/obs/job.h"
#include "xdec/obs/monitor.h"
#include "xdec/obs/recorder.h"
#include "xdec/obs/sinks.h"
#include "xdec/support/diag.h"
#include "xdec/support/json.h"

namespace xdec::cli {

struct GlobalOptions {
  enum class Progress : uint8_t { Auto, Always, Never };
  enum class OnTimeout : uint8_t { Partial, Abort };
  enum class LogFormat : uint8_t { Auto, Text, Json };

  std::vector<std::string> logSpecs;
  std::filesystem::path logFile;
  LogFormat logFormat = LogFormat::Auto;
  std::filesystem::path tracePath;
  std::filesystem::path reportPath;
  std::filesystem::path crashDir;
  Progress progress = Progress::Auto;
  std::optional<std::chrono::nanoseconds> timeout;
  std::optional<std::chrono::nanoseconds> hardTimeout;
  OnTimeout onTimeout = OnTimeout::Partial;
  std::chrono::nanoseconds watchdog = std::chrono::seconds{120};
  bool quiet = false;
  bool verbose = false;
  bool machine = false;
  bool profile = false;
};

class RunContext {
 public:
  RunContext();
  ~RunContext();

  RunContext(const RunContext&) = delete;
  RunContext& operator=(const RunContext&) = delete;

  /// The instance main() created, or null (tests, other hosts).
  [[nodiscard]] static RunContext* current() noexcept;

  /// Removes the global options from `args` and applies them. Returns an
  /// error message for a malformed option.
  [[nodiscard]] std::optional<std::string> configure(std::vector<std::string_view>& args);

  /// Installs sinks, handlers and the monitor. Call before the command runs.
  void start(std::span<const std::string_view> commandLine);
  /// Stops the monitor, writes the report and trace, prints the summary.
  void finish(int exitCode);

  [[nodiscard]] obs::Job& job() noexcept { return job_; }
  [[nodiscard]] const GlobalOptions& options() const noexcept { return options_; }

  /// A command's own part of the report (`decompile`, say).
  void setSection(std::string key, json::Value value);
  /// Overrides the outcome derived from the exit code ("partial", say).
  void setOutcome(std::string outcome);
  /// Records a diagnostic the command reported (reportError calls this).
  void noteDiag(const Diag& diag);

 private:
  [[nodiscard]] json::Value buildReport(int exitCode) const;
  [[nodiscard]] std::string outcomeFor(int exitCode) const;

  GlobalOptions options_;
  obs::Job job_{"xdec"};
  std::vector<std::string> commandLine_;
  int64_t startWallUs_ = 0;
  std::shared_ptr<obs::RunRecorder> recorder_;
  std::shared_ptr<obs::RingBufferSink> recent_;
  std::shared_ptr<obs::ChromeTraceSink> trace_;
  std::unique_ptr<obs::Monitor> monitor_;
  std::vector<std::pair<std::string, json::Value>> sections_;
  std::string outcome_;
  std::optional<Diag> failure_;
  bool finished_ = false;
};

}  // namespace xdec::cli
