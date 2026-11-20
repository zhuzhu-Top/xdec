#include "common.h"

#include <charconv>
#include <cstdio>

#include "run_context.h"

namespace xdec::cli {

XDEC_DEFINE_LOG_CATEGORY_LEVEL(cliLog, "cli", ::xdec::obs::LogLevel::Info)

void printLine(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

namespace detail {
void say(obs::LogLevel level, std::string message) {
  obs::LogCategory& category = cliLog();
  if (category.enabled(level)) {
    obs::detail::emit(category, level, std::source_location::current(), {}, std::move(message));
  }
}
}  // namespace detail

bool parseNumber(std::string_view text, uint64_t& out) {
  int base = 10;
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty()) {
    return false;
  }
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out, base);
  return result.ec == std::errc{} && result.ptr == end;
}

int reportError(const xdec::Diag& diag) {
  printError("{}", diag.format());
  if (RunContext* run = RunContext::current(); run != nullptr) {
    run->noteDiag(diag);
  }
  switch (diag.code()) {
    case DiagCode::Cancelled:
      return 130;
    case DiagCode::Timeout:
      return 124;
    default:
      return 1;
  }
}

int usage() {
  printLine("usage: xdec <command> [arguments]");
  printLine("");
  printLine("commands:");
  printLine("  info <binary>                    summarise an image");
  printLine("  images <cache> [count]           list a dyld shared cache's indexed images (dylibs)");
  printLine("  cache-locate <cache> <address>   which image/symbol a dyld shared cache address belongs to");
  printLine("  sections <binary>                list sections");
  printLine("  symbols <binary> [opts]          list symbols (--grep, --kind func|import|export|data, --limit)");
  printLine("  functions <binary> [opts]        list functions & ObjC methods (--grep, --limit)");
  printLine("  objc <binary> [opts]             inspect Mach-O ObjC classes/ivars/methods (--class, --sel, --limit)");
  printLine("  strings <binary> [opts]          scan strings (--grep, --min-len, --section, --limit, --xrefs)");
  printLine("  xrefs <binary> <target> [opts]   find ARM64 references (addr, sym, sel:<name>, str:<text>)");
  printLine("  callers <binary> <target> [opts] trace callers up to --depth N");
  printLine("  callees <binary> <fn> [opts]     list direct/stub callees inside a function");
  printLine("  data <binary> <addr> [count]     inspect structured data / CFString / selref / pointer table");
  printLine("  serve <binary> [--mcp] [opts]    persistent stdio JSON-RPC / MCP server (keeps image in memory)");
  printLine("  relocs <binary> [count]          list relocations");
  printLine("  read <binary> <address> <size>   hex dump the unified memory view");
  printLine("  spec <file.xspec> [out.bin]    check a spec, optionally emitting a blob");
  printLine("  disasm <binary> <address> <n>    disassemble n instructions");
  printLine("  lift <binary> <address> <n>      lift n instructions and print the IL");
  printLine("  observe <binary> <address> [...] lift a function, run passes, dump each step");
  printLine("      options: --to <maturity> --out <dir> --plugin <path>");
  printLine("  decompile <binary> <addr|-s sym|--match pat> [...] lift, resolve, emit C");
  printLine("      options: -o <file.c> --out-dir <dir> -j <threads> --engine <tracedb-dir>");
  printLine("               --types <header|preset> (repeatable)");
  printLine("               --syscall-table <file|name|none> (default aarch64-linux)");
  printLine("               --plugin <path> (repeatable)");
  printLine("  exec <binary> <workload>         execute blocks against scripted states");
  printLine("  memdump <binary> <out>           dump the relocated memory view for emulators");
  printLine("  decode                           decode hex words from stdin (fuzzer iface)");
  printLine("  types parse <header|preset>...   import C declarations and report them");
  printLine("      options: -o <out.json> --definitions");
  printLine("  coverage <binary> [rows]         report what the spec does not decode");
  printLine("  log-categories                   list logging categories");
  printLine("");
  printLine("run options (any command, anywhere on the line):");
  printLine("  --log <spec>          category levels, e.g. driver=info,pass=debug (repeatable;");
  printLine("                        same grammar as XDEC_LOG, applied after it)");
  printLine("  -v, --verbose         every category at info");
  printLine("  -q, --quiet           only warnings and errors on stderr");
  printLine("  --log-file <path>     also write the log there (.jsonl/.json: JSON Lines)");
  printLine("  --log-format <text|json>  override the log file format");
  printLine("  --trace <path>        Chrome trace of every span (open in ui.perfetto.dev)");
  printLine("  --profile             print a per-span timing table to stderr at the end");
  printLine("  --report <path>       JSON run report: outcome, timings, counters, memory, warnings");
  printLine("  --progress <auto|always|never>  live status line on stderr (auto: when a TTY)");
  printLine("  --timeout <dur>       stop discovering after <dur> and emit what was proved");
  printLine("  --on-timeout <partial|abort>  partial (default) allows a grace period to finish");
  printLine("  --hard-timeout <dur>  cancel outright after <dur> (default: timeout + max(30s, timeout/2))");
  printLine("  --watchdog <dur>      report open spans when nothing progresses for <dur> (default 120s, 0 off)");
  printLine("  --machine             JSON Lines log/progress/result on stderr, for GUIs and scripts");
  printLine("  --crash-dir <dir>     where a crash report is written (default: current directory)");
  printLine("  durations: 1500ms, 90s, 5m, 1h. Ctrl+C once: wind down to a partial result; twice: cancel.");
  printLine("");
  printLine("stdout carries only the command's result; status, warnings and errors go to stderr.");
  printLine("Set XDEC_SPEC=<file.xspec> to override the architecture spec.");
  return 2;
}

}  // namespace xdec::cli
