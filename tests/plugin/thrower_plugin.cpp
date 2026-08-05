// A plugin whose init throws across the ABI boundary. The loader must report
// it as a PluginError diagnostic, never let the exception unwind into the
// host.
#include <stdexcept>

#include "xdec/plugin/abi.h"

extern "C" XDEC_EXPORT std::uint32_t xdec_plugin_abi_version() {
  return XDEC_PLUGIN_ABI_VERSION;
}

extern "C" XDEC_EXPORT XdecPluginInitResult
xdec_plugin_init(xdec::pass::Registry*) {
  throw std::runtime_error("a plugin bug, deliberately uncaught");
}
