#include "xdec/support/paths.h"

#include <array>
#include <cstdint>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace xdec {

std::filesystem::path executableDirectory() {
  std::error_code ec;
#if defined(_WIN32)
  std::wstring buf(1024, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0) {
      return {};
    }
    if (len < buf.size()) {
      buf.resize(len);
      return std::filesystem::path{buf}.parent_path();
    }
    if (buf.size() >= 32768) {
      return {};
    }
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (size == 0) {
    return {};
  }
  std::vector<char> buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    return {};
  }
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(buf.data(), ec);
  return (ec ? std::filesystem::path{buf.data()} : canonical).parent_path();
#elif defined(__linux__)
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || self.empty()) {
    return {};
  }
  return self.parent_path();
#else
  return {};
#endif
}

std::filesystem::path findShippedDataFile(
    const std::filesystem::path& relativeFromRoot, std::string_view compiledDir,
    const std::filesystem::path& relativeFromCompiledDir) {
  std::error_code ec;
  const std::filesystem::path exeDir = executableDirectory();
  if (!exeDir.empty()) {
    const std::array<std::filesystem::path, 3> candidates = {
        exeDir / relativeFromRoot,
        exeDir.parent_path() / relativeFromRoot,
        exeDir.parent_path() / "share" / "xdec" / relativeFromRoot,
    };
    for (const std::filesystem::path& cand : candidates) {
      if (std::filesystem::is_regular_file(cand, ec) && !ec) {
        return cand;
      }
      ec.clear();
    }
  }
  return std::filesystem::path{compiledDir} / relativeFromCompiledDir;
}

std::string defaultPluginPath(std::string_view stem) {
  const std::filesystem::path exeDir = executableDirectory();
  if (exeDir.empty()) {
    return {};
  }
  const std::string base{stem};
#if defined(_WIN32)
  const std::array<std::filesystem::path, 2> candidates = {
      exeDir / (base + ".dll"),
      exeDir.parent_path() / "lib" / (base + ".dll"),
  };
#elif defined(__APPLE__)
  const std::array<std::filesystem::path, 4> candidates = {
      exeDir / (base + ".dylib"),
      exeDir / ("lib" + base + ".dylib"),
      exeDir.parent_path() / "lib" / (base + ".dylib"),
      exeDir.parent_path() / "lib" / ("lib" + base + ".dylib"),
  };
#else
  const std::array<std::filesystem::path, 4> candidates = {
      exeDir / (base + ".so"),
      exeDir / ("lib" + base + ".so"),
      exeDir.parent_path() / "lib" / (base + ".so"),
      exeDir.parent_path() / "lib" / ("lib" + base + ".so"),
  };
#endif
  std::error_code ec;
  for (const std::filesystem::path& cand : candidates) {
    if (std::filesystem::is_regular_file(cand, ec) && !ec) {
      return cand.string();
    }
    ec.clear();
  }
  return {};
}

}  // namespace xdec
