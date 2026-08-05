// Textual IL.
//
// The text form is the primary debugging surface and also the format the parser
// reads back, so printing and parsing are held to an exact round trip:
// print(parse(print(f))) == print(f). That equality is a real test, not an
// aspiration -- it catches printer and parser drift the moment it appears, and
// every new op has to survive it.
//
// Grammar, informally:
//
//   function @<entry> name="<name>" arch=<arch> maturity=<level> {
//     block b0 @0x1000..0x1010 preds=[] {
//       @0x1000
//       %0 = read x29
//       write sp, sub:i64(val:i64(%0), const:i64(0x60))
//       brc flagcond:eq(val:flags(%3)), b1, b2
//     }
//   }
//
// An `@<address>` line sets the source address of the ops that follow, so
// provenance round-trips without repeating an address per line. Expressions are
// printed as `<op>[:<modifier>](<args>)`; the colon separates the op name from
// its modifier because op names themselves contain dots (`cmp.eq`, `shr.u`).
#pragma once

#include <string>

#include "xdec/il/function.h"

namespace xdec::il {

struct PrintOptions {
  /// Include the source disassembly, when the caller supplied it, as comments.
  bool includeComments = true;
  /// Print derived predecessor lists. They are recomputed on parse, so this is
  /// for human readers only.
  bool includePredecessors = true;
  /// Number of spaces per indentation level.
  unsigned indent = 2;
};

/// Renders the whole function.
[[nodiscard]] std::string print(const Function& function, const PrintOptions& options = {});

/// Renders one block, for pass-level dumps.
[[nodiscard]] std::string printBlock(const Function& function, BlockId block,
                                     const PrintOptions& options = {});

/// Renders one expression tree.
[[nodiscard]] std::string printExpr(const Function& function, ExprId expr);

/// Renders one operation, without the leading address marker.
[[nodiscard]] std::string printOp(const Function& function, OpId op);

}  // namespace xdec::il
