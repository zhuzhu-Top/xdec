#include "common.h"

#include <charconv>
#include <cstdio>

namespace xdec::cli {

void printLine(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

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
  print("error: {}", diag.format());
  return 1;
}

int usage() {
  printLine("usage: xdec <command> [arguments]");
  printLine("");
  printLine("commands:");
  printLine("  info <binary>                    summarise an image");
  printLine("  sections <binary>                list sections");
  printLine("  symbols <binary> [count]         list defined symbols by address");
  printLine("  relocs <binary> [count]          list relocations");
  printLine("  read <binary> <address> <size>   hex dump the unified memory view");
  printLine("  spec <file.xspec> [out.bin]    check a spec, optionally emitting a blob");
  printLine("  disasm <binary> <address> <n>    disassemble n instructions");
  printLine("  lift <binary> <address> <n>      lift n instructions and print the IL");
  printLine("  observe <binary> <address> [...] lift a function, run passes, dump each step");
  printLine("      options: --to <maturity> --out <dir> --plugin <path>");
  printLine("  decompile <binary> <address> [...] the full pipeline: lift, resolve, emit C");
  printLine("      options: -o <file.c> --rounds <n> --no-annotate --allow-unresolved");
  printLine("               --types <header|preset> (repeatable)");
  printLine("               --syscall-table <file|name|none> (default aarch64-linux)");
  printLine("               --reuse-report (count same-block subexpression duplication)");
  printLine("               --emit-report (count IL-level redundant-temp shapes)");
  printLine("               --dump-il (print the IL after all passes, before structuring)");
  printLine("               --helpers-header <path|none> (default xdec_helpers.h)");
  printLine("               --arg-naming <indexed|reg> (default indexed: arg1, arg2, ...)");
  printLine("               --security-hints <comment|keep> (default comment)");
  printLine("               --region-structuring (J2 diagnostic: collapse a nested dispatch");
  printLine("                 region's tree into fewer switches; default off)");
  printLine("  exec <binary> <workload>         execute blocks against scripted states");
  printLine("  memdump <binary> <out>           dump the relocated memory view for emulators");
  printLine("  decode                           decode hex words from stdin (fuzzer iface)");
  printLine("  types parse <header|preset>...   import C declarations and report them");
  printLine("      options: -o <out.json> --definitions");
  printLine("  coverage <binary> [rows]         report what the spec does not decode");
  printLine("  log-categories                   list logging categories");
  printLine("");
  printLine("Set XDEC_LOG=<category>=<level> for diagnostics, e.g. XDEC_LOG=pass=debug,local=debug.");
  printLine("Set XDEC_SPEC=<file.xspec> to override the architecture spec.");
  return 2;
}

}  // namespace xdec::cli
