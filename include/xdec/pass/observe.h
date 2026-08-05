// Per-pass pipeline observation: dump and op-census files.
//
// DumpObserver is the debugging surface of the pass framework. For a pipeline
// of N passes it writes, into one directory:
//
//   00-<maturity>.il / .map   the function as it entered the pipeline;
//   NN-<pass>.il     / .map   the function after each pass, numbered so plain
//                             lexicographic order is pipeline order;
//   index.txt                 one line per pass: level walk, iteration count,
//                             change flag, op count before and after.
//
// The .map is the op census: one `op #<id> @<va> from <pass>` line per live
// op. Op ids are stable across passes, so diffing two consecutive censuses
// names exactly the ops a pass created and deleted; on a maturity jump the
// pair of censuses around it is the jump-level mapping, with machine
// addresses as the anchors that tie the two levels together. Text diff on
// the .il pair answers "what did the pass change"; census diff answers "which
// ops survived", which is the question a level jump actually raises.
//
// Dumping never fails the pipeline: a broken output directory records one
// diagnostic here instead of aborting the run the user asked for.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "xdec/il/function.h"
#include "xdec/pass/manager.h"
#include "xdec/support/result.h"

namespace xdec::pass {

class DumpObserver final : public Observer {
 public:
  explicit DumpObserver(std::filesystem::path outDir);

  void beforePass(const Pass& pass, const il::Function& function) override;
  void afterPass(const Pass& pass, const il::Function& function, const RunStats& stats) override;
  void pipelineDone(const il::Function& function) override;

  /// The first failure encountered while writing, if any.
  [[nodiscard]] const std::optional<Diag>& failure() const noexcept { return failure_; }

 private:
  void dumpState(const il::Function& function, const std::string& stem);
  void record(Diag diag);

  std::filesystem::path outDir_;
  unsigned next_ = 1;
  std::size_t lastOps_ = 0;
  std::vector<std::string> index_;
  std::optional<Diag> failure_;
};

}  // namespace xdec::pass
