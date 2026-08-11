// The xdec command line entry point. Argument parsing and every command's
// implementation live under cli/; this just turns argv into a dispatch call.
#include <string_view>
#include <vector>

#include "cli/dispatch.h"

int main(int argc, char** argv) {
  const std::vector<std::string_view> args{argv + 1, argv + argc};
  return xdec::cli::dispatch(args);
}
