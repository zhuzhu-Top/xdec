// The plugin ABI: the narrow, versioned boundary between host and plugin.
//
// What crosses this boundary is exactly three unmangled symbols and a version
// number. Everything else — including the xdec::pass::Registry the plugin
// registers its passes into — is C++ from the SDK headers, which is a
// deliberate choice: plugins are built with the same compiler and SDK as the
// host (documented in docs/), and pretending a C++ IL graph fits through a
// char* interface would buy imaginary portability at the cost of a parallel
// handle-based API nobody wants to write passes against.
//
// The contract:
//
//   1. The plugin exports `xdec_plugin_abi_version`, returning
//      XDEC_PLUGIN_ABI_VERSION it was compiled against. The host refuses any
//      plugin whose version does not match its own — a mismatch means the
//      PassInfo layout, the Maturity enum, or the Result convention may have
//      changed, and loading anyway would fail somewhere less explainable.
//
//   2. The plugin exports `xdec_plugin_init`, which registers its passes into
//      the handed registry and reports success or a static error message.
//
//   3. Lifetime: the library must remain loaded for as long as any pass it
//      registered can still run. The loader keeps the handle; unloading early
//      leaves dangling vtables, and there is no refcounting scheme that makes
//      that safe — so the rule is simply: plugins unload at process shutdown.
//
// Crash containment: init and pass runs happen inside the host's try/catch at
// the call sites the loader provides; a plugin that throws across the boundary
// is reported as a PluginError diagnostic, not a process crash. (Hardware
// faults are out of scope — that would require a worker process, which the
// design deliberately does not take on yet.)
#pragma once

#include <cstdint>

#include "xdec/pass/registry.h"
#include "xdec/support/compiler.h"

/// Bumped on any change to what a plugin sees: PassInfo layout, Maturity
/// values, registration protocol.
inline constexpr std::uint32_t XDEC_PLUGIN_ABI_VERSION = 1;

extern "C" {

/// Export names, as string constants, so the loader and the plugin cannot
/// drift apart by a typo that compiles fine on both sides.
inline constexpr const char* XDEC_PLUGIN_VERSION_SYMBOL = "xdec_plugin_abi_version";
inline constexpr const char* XDEC_PLUGIN_INIT_SYMBOL = "xdec_plugin_init";

/// Result of the init call. `message` must point to static storage inside the
/// plugin (a string literal); it is read only when `ok` is false.
struct XdecPluginInitResult {
  bool ok;
  const char* message;
};

using XdecPluginAbiVersionFn = std::uint32_t (*)();
using XdecPluginInitFn = XdecPluginInitResult (*)(xdec::pass::Registry* registry);

}  // extern "C"

/// What a plugin definition boils down to, for the plugin author's side:
///
///   extern "C" XDEC_EXPORT std::uint32_t xdec_plugin_abi_version() {
///     return XDEC_PLUGIN_ABI_VERSION;
///   }
///   extern "C" XDEC_EXPORT XdecPluginInitResult
///   xdec_plugin_init(xdec::pass::Registry* registry) {
///     ... register passes ...
///     return {true, nullptr};
///   }
