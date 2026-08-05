#include "xdec/plugin/loader.h"

#include <exception>
#include <format>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include "xdec/plugin/abi.h"

namespace xdec::plugin {

namespace {

struct Library {
  void* handle = nullptr;

  Library() = default;
  explicit Library(void* rawHandle) : handle(rawHandle) {}
  // Owning: no copies. The destructor suppresses the implicit move, so it is
  // declared by hand — a silent copy here double-frees the handle.
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  Library(Library&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
  Library& operator=(Library&& other) noexcept {
    if (this != &other) {
      this->~Library();
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }

  ~Library() {
    if (handle != nullptr) {
#ifdef _WIN32
      FreeLibrary(static_cast<HMODULE>(handle));
#else
      dlclose(handle);
#endif
    }
  }

  [[nodiscard]] void* symbol(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
  }
};

[[nodiscard]] Result<Library> openLibrary(const std::filesystem::path& path) {
#ifdef _WIN32
  HMODULE handle = LoadLibraryW(path.c_str());
  if (handle == nullptr) {
    return err(DiagCode::PluginError, "cannot load plugin '{}': system error {}",
               path.generic_string(), GetLastError());
  }
  return Library{static_cast<void*>(handle)};
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return err(DiagCode::PluginError, "cannot load plugin '{}': {}", path.generic_string(),
               dlerror());
  }
  return Library{handle};
#endif
}

}  // namespace

Plugin& Plugin::operator=(Plugin&& other) noexcept {
  if (this != &other) {
    unload();
    handle_ = other.handle_;
    registered_ = other.registered_;
    other.handle_ = nullptr;
    other.registered_ = 0;
  }
  return *this;
}

Plugin::~Plugin() { unload(); }

void Plugin::unload() noexcept {
  if (handle_ != nullptr) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
    registered_ = 0;
  }
}

Result<Plugin> Plugin::load(const std::filesystem::path& path, pass::Registry& registry) {
  XDEC_TRY(Library library, openLibrary(path));

  // Version handshake before anything else runs: a plugin compiled against a
  // different SDK layout must never get to execute code that assumes the new
  // one.
  void* versionSymbol = library.symbol(XDEC_PLUGIN_VERSION_SYMBOL);
  if (versionSymbol == nullptr) {
    return err(DiagCode::PluginError, "plugin '{}' exports no {} — not an xdec plugin",
               path.generic_string(), XDEC_PLUGIN_VERSION_SYMBOL);
  }
  const auto abiVersion = reinterpret_cast<XdecPluginAbiVersionFn>(versionSymbol)();
  if (abiVersion != XDEC_PLUGIN_ABI_VERSION) {
    return err(DiagCode::PluginError,
               "plugin '{}' was built against ABI version {}, but the host speaks {}",
               path.generic_string(), abiVersion, XDEC_PLUGIN_ABI_VERSION);
  }

  void* initSymbol = library.symbol(XDEC_PLUGIN_INIT_SYMBOL);
  if (initSymbol == nullptr) {
    return err(DiagCode::PluginError, "plugin '{}' exports no {}", path.generic_string(),
               XDEC_PLUGIN_INIT_SYMBOL);
  }

  // The plugin's C++ runs here; an exception escaping the boundary is a plugin
  // bug, reported as a diagnostic rather than allowed to unwind into the host.
  const std::size_t before = registry.size();
  XdecPluginInitResult init{false, "init never returned"};
  try {
    init = reinterpret_cast<XdecPluginInitFn>(initSymbol)(&registry);
  } catch (const std::exception& exception) {
    return err(DiagCode::PluginError, "plugin '{}' init threw: {}", path.generic_string(),
               exception.what());
  } catch (...) {
    return err(DiagCode::PluginError, "plugin '{}' init threw an unknown exception",
               path.generic_string());
  }
  if (!init.ok) {
    return err(DiagCode::PluginError, "plugin '{}' init failed: {}", path.generic_string(),
               init.message != nullptr ? init.message : "(no message)");
  }

  Plugin plugin;
  plugin.handle_ = library.handle;
  plugin.registered_ = registry.size() - before;
  library.handle = nullptr;  // ownership moves to the Plugin
  return plugin;
}

}  // namespace xdec::plugin
