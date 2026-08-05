// Plugin loading: the version handshake, init, error paths, and end-to-end
// execution of a plugin-registered pass through the manager.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "il/il_test_support.h"
#include "xdec/il/function.h"
#include "xdec/pass/manager.h"
#include "xdec/pass/registry.h"
#include "xdec/plugin/loader.h"

using xdec::Arch;
using xdec::il::BlockId;
using xdec::il::Function;
using xdec::il::Maturity;
using xdec::pass::Manager;
using xdec::pass::Registry;
using xdec::plugin::Plugin;

#ifndef XDEC_TEST_PLUGIN_ECHO
#  error "XDEC_TEST_PLUGIN_ECHO must name the built echo plugin library"
#endif
#ifndef XDEC_TEST_PLUGIN_OLD_ABI
#  error "XDEC_TEST_PLUGIN_OLD_ABI must name the stale-ABI plugin library"
#endif
#ifndef XDEC_TEST_PLUGIN_THROWER
#  error "XDEC_TEST_PLUGIN_THROWER must name the throwing plugin library"
#endif

namespace {

[[nodiscard]] Function makeFunction() {
  Function function(Arch::AArch64, xdec::test::arm64Registers(), 0x1000);
  const BlockId entry = function.createBlock(0x1000);
  function.setEntryBlock(entry);
  const xdec::il::RegId x0 = function.registers().find("x0");
  const xdec::il::ValueId value = function.appendReadReg(entry, 0x1000, x0);
  function.appendWriteReg(entry, 0x1004, x0, function.valueRef(value));
  return function;
}

}  // namespace

TEST_CASE("a plugin loads and its pass runs with provenance", "[plugin]") {
  // Declaration order matters: the Plugin must outlive the Registry that owns
  // its passes, so it unloads only after they are destroyed (see abi.h).
  Plugin plugin;
  Registry registry;
  auto loaded = Plugin::load(XDEC_TEST_PLUGIN_ECHO, registry);
  const std::string loadError = loaded ? std::string{} : loaded.error().format();
  INFO(loadError);
  REQUIRE(loaded);
  plugin = std::move(*loaded);
  CHECK(plugin.registeredPasses() == 1);
  CHECK(registry.find("plugin-nop") != nullptr);

  Function function = makeFunction();
  Manager manager;
  const auto stats = manager.runTo(function, registry, Maturity::Lifted);
  const std::string statsError = stats ? std::string{} : stats.error().format();
  INFO(statsError);
  REQUIRE(stats);

  const BlockId entry = function.entryBlock();
  const xdec::il::OpId last = function.block(entry).ops.back();
  CHECK(function.passName(function.op(last).origin) == "plugin-nop");
}

TEST_CASE("a plugin built against another ABI version is refused", "[plugin]") {
  Registry registry;
  const auto plugin = Plugin::load(XDEC_TEST_PLUGIN_OLD_ABI, registry);
  REQUIRE_FALSE(plugin);
  CHECK(plugin.error().code() == xdec::DiagCode::PluginError);
  CHECK(plugin.error().format().find("ABI version") != std::string::npos);
  // A refused plugin must leave the registry untouched.
  CHECK(registry.size() == 0);
}

TEST_CASE("an exception escaping init is a diagnostic, not a crash", "[plugin]") {
  Registry registry;
  const auto plugin = Plugin::load(XDEC_TEST_PLUGIN_THROWER, registry);
  REQUIRE_FALSE(plugin);
  CHECK(plugin.error().code() == xdec::DiagCode::PluginError);
  CHECK(plugin.error().format().find("threw") != std::string::npos);
}

TEST_CASE("a file that is not there fails cleanly", "[plugin]") {
  Registry registry;
  const auto plugin =
      Plugin::load(std::filesystem::path{"definitely-not-a-plugin.dll"}, registry);
  REQUIRE_FALSE(plugin);
  CHECK(plugin.error().code() == xdec::DiagCode::PluginError);
}
