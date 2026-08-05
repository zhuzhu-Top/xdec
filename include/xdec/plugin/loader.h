// Dynamic plugin loading.
//
// A Plugin owns the loaded library handle and, through it, the truth that the
// code inside is reachable. The passes the plugin registered live in the
// Registry it was handed; the Plugin must therefore outlive every pipeline
// that could run those passes (see the lifetime rule in abi.h). Move-only,
// destructor unloads: one owner, one handle, no refcounting games.
#pragma once

#include <filesystem>
#include <string>

#include "xdec/pass/registry.h"
#include "xdec/support/result.h"

namespace xdec::plugin {

class Plugin {
 public:
  Plugin() noexcept = default;
  Plugin(const Plugin&) = delete;
  Plugin& operator=(const Plugin&) = delete;
  Plugin(Plugin&& other) noexcept { *this = std::move(other); }
  Plugin& operator=(Plugin&& other) noexcept;
  ~Plugin();

  /// Loads the library, checks the ABI version, runs its init against
  /// `registry`. On any failure the registry is left untouched: version and
  /// symbol checks happen before init, and an init that reports failure is
  /// expected to have registered nothing (a plugin that half-registers and
  /// then fails is violating its own contract).
  [[nodiscard]] static Result<Plugin> load(const std::filesystem::path& path,
                                           pass::Registry& registry);

  [[nodiscard]] bool loaded() const noexcept { return handle_ != nullptr; }
  /// The number of passes the plugin registered (recorded at load time, for
  /// diagnostics and tests).
  [[nodiscard]] std::size_t registeredPasses() const noexcept { return registered_; }

  /// Unloads explicitly. Idempotent; the destructor calls this. After unload
  /// the plugin's passes must never run again — see abi.h.
  void unload() noexcept;

 private:
  void* handle_ = nullptr;
  std::size_t registered_ = 0;
};

}  // namespace xdec::plugin
