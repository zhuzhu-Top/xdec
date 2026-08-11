// Command-line option parsing shared by more than one command. Kept small
// deliberately: most of `observe` and `decompile`'s options are not actually
// duplicated between the two, so only the one that is (`--rounds`) lives here.
#pragma once

#include <string_view>

namespace xdec::cli {

/// A round count the user chose is a wall, not a budget: they asked for
/// bounded work (see decompile::DriverOptions::extendWhileProving).
struct RoundCap {
  unsigned value = 8;
  bool pinned = false;
};

/// Parses a `--rounds` value shared by `observe` and `decompile`: a decimal
/// or hex count between 1 and 1024, which also pins the cap so the driver
/// stops there instead of extending past it while still proving new blocks.
[[nodiscard]] bool parseRoundCap(std::string_view text, RoundCap& out);

}  // namespace xdec::cli
