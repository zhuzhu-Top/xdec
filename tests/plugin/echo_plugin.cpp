// A minimal but real plugin: one pass, registered through the ABI boundary.
//
// Compiled twice by the test build: once straight (loads fine), once with
// XDEC_PLUGIN_FAKE_VERSION (must be refused). The pass appends a Nop so the
// test can observe both the effect and the provenance tag.
#include <memory>

#include "xdec/il/function.h"
#include "xdec/plugin/abi.h"

namespace {

class PluginNop final : public xdec::pass::FunctionPass {
 public:
  PluginNop()
      : FunctionPass([] {
          xdec::pass::PassInfo info;
          info.name = "plugin-nop";
          info.level = xdec::il::Maturity::Lifted;
          info.produces = xdec::il::Maturity::Lifted;
          return info;
        }()) {}

  xdec::Result<bool> run(xdec::pass::Context& context) override {
    xdec::il::Function& function = context.function();
    // Appending after a terminator is an IL violation, and a real function's
    // entry block is terminated: park the Nop there only while it is not.
    const xdec::il::BlockId entry = function.entryBlock();
    const xdec::il::OpId last = function.block(entry).terminator();
    if (last.valid() && function.op(last).isTerminator()) {
      return false;
    }
    function.appendNop(entry, 0x4000);
    return true;
  }
};

}  // namespace

extern "C" XDEC_EXPORT std::uint32_t xdec_plugin_abi_version() {
#ifdef XDEC_PLUGIN_FAKE_VERSION
  return XDEC_PLUGIN_FAKE_VERSION;
#endif
  return XDEC_PLUGIN_ABI_VERSION;
}

extern "C" XDEC_EXPORT XdecPluginInitResult
xdec_plugin_init(xdec::pass::Registry* registry) {
  const xdec::Result<void> added = registry->add(std::make_unique<PluginNop>());
  if (!added) {
    return {false, "registration failed"};
  }
  return {true, nullptr};
}
