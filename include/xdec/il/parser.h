// Reads the textual IL produced by the printer.
//
// The parser exists for three reasons, in order of importance: it makes IL text
// the test fixture format, so a regression is a text file rather than a hundred
// lines of builder calls; it turns printer and parser agreement into a
// mechanically checkable property; and it lets external tooling hand IL back.
#pragma once

#include <memory>
#include <string_view>

#include "xdec/il/function.h"
#include "xdec/support/result.h"

namespace xdec::il {

/// Parses a complete function. The register file must already describe every
/// register the text names; an unknown register name is an error rather than a
/// silently invented register.
Result<std::unique_ptr<Function>> parse(std::string_view text, const RegisterFile& registers);

}  // namespace xdec::il
