// DumpObserver: see observe.h for the file layout it produces.
#include "xdec/pass/observe.h"

#include <format>
#include <fstream>

#include "xdec/il/printer.h"

namespace xdec::pass {

namespace {

/// One census line per live op: id, machine address (or `-` for synthesized
/// ops), and the pass that created it.
[[nodiscard]] std::string census(const il::Function& function) {
  std::string out;
  for (uint32_t raw = 0; raw < function.opCount(); ++raw) {
    const il::OpId id{raw};
    if (!function.hasOp(id)) {
      continue;
    }
    const il::Op& op = function.op(id);
    if (op.va == il::kNoOpAddress) {
      out += std::format("op #{} @- from {}\n", raw, function.passName(op.origin));
    } else {
      out += std::format("op #{} @0x{:x} from {}\n", raw, op.va, function.passName(op.origin));
    }
  }
  return out;
}

}  // namespace

DumpObserver::DumpObserver(std::filesystem::path outDir) : outDir_(std::move(outDir)) {
  std::error_code ec;
  std::filesystem::create_directories(outDir_, ec);
  if (ec) {
    record(Diag{DiagCode::IoError,
                std::format("cannot create observe directory '{}': {}", outDir_.string(),
                            ec.message())});
  }
}

void DumpObserver::record(Diag diag) {
  if (!failure_) {
    failure_ = std::move(diag);
  }
}

void DumpObserver::dumpState(const il::Function& function, const std::string& stem) {
  if (failure_) {
    return;
  }
  {
    std::ofstream il(outDir_ / (stem + ".il"), std::ios::binary | std::ios::trunc);
    if (!il) {
      record(Diag{DiagCode::IoError, std::format("cannot write '{}.il'", stem)});
      return;
    }
    il << il::print(function);
  }
  {
    std::ofstream map(outDir_ / (stem + ".map"), std::ios::binary | std::ios::trunc);
    if (!map) {
      record(Diag{DiagCode::IoError, std::format("cannot write '{}.map'", stem)});
      return;
    }
    map << census(function);
  }
}

void DumpObserver::beforePass(const Pass& pass, const il::Function& function) {
  (void)pass;
  if (next_ == 1 && lastOps_ == 0) {
    // First hook of the run: capture the state the pipeline started from.
    dumpState(function, std::format("00-{}", toString(function.maturity())));
  }
  lastOps_ = function.opCount();
}

void DumpObserver::afterPass(const Pass& pass, const il::Function& function,
                             const RunStats& stats) {
  const PassInfo& info = pass.info();
  dumpState(function, std::format("{:02}-{}", next_, info.name));
  index_.push_back(std::format("{:02} {} {}->{} iter={} changed={} ops={}->{}", next_,
                               info.name, toString(info.level), toString(info.produces),
                               stats.iterations, stats.changed ? "true" : "false", lastOps_,
                               function.opCount()));
  lastOps_ = function.opCount();
  ++next_;
}

void DumpObserver::pipelineDone(const il::Function& function) {
  (void)function;
  if (failure_) {
    return;
  }
  std::ofstream index(outDir_ / "index.txt", std::ios::binary | std::ios::trunc);
  if (!index) {
    record(Diag{DiagCode::IoError, "cannot write 'index.txt'"});
    return;
  }
  for (const std::string& line : index_) {
    index << line << '\n';
  }
}

}  // namespace xdec::pass
