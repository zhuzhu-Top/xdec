# Project-wide build options.

option(XDEC_BUILD_TESTS "Build the xdec test suite" ON)
option(XDEC_WERROR "Treat compiler warnings as errors" OFF)
option(XDEC_ENABLE_LTO "Enable link-time optimization for release builds" OFF)

set(XDEC_SANITIZERS "" CACHE STRING
  "Semicolon-separated sanitizers to enable (address;undefined;thread)")

# MinGW targets have no usable ASan/TSan runtime; fail loudly rather than
# producing a build that silently checks nothing.
if(XDEC_SANITIZERS AND MINGW)
  message(FATAL_ERROR
    "XDEC_SANITIZERS is set but sanitizer runtimes are unavailable on MinGW. "
    "Use a Linux or MSVC toolchain for sanitizer builds.")
endif()

add_library(xdec_options INTERFACE)

if(XDEC_SANITIZERS)
  list(JOIN XDEC_SANITIZERS "," _xdec_san)
  target_compile_options(xdec_options INTERFACE
    -fsanitize=${_xdec_san} -fno-omit-frame-pointer -g)
  target_link_options(xdec_options INTERFACE -fsanitize=${_xdec_san})
endif()

if(XDEC_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _xdec_lto OUTPUT _xdec_lto_msg)
  if(_xdec_lto)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON PARENT_SCOPE)
  else()
    message(WARNING "LTO requested but unsupported: ${_xdec_lto_msg}")
  endif()
endif()

# Large translation units (generated decode tables) exceed the default
# MinGW section limit, and the ELF/DSL code relies on wide integer literals.
if(MINGW)
  target_compile_options(xdec_options INTERFACE -Wa,-mbig-obj)
endif()
