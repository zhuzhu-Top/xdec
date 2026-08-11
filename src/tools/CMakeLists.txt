xdec_add_executable(xdec
  SOURCES
    main.cpp
    cli/cmd_exec.cpp
    cli/cmd_image.cpp
    cli/cmd_lift.cpp
    cli/cmd_misc.cpp
    cli/cmd_pipeline.cpp
    cli/cmd_spec.cpp
    cli/cmd_types.cpp
    cli/common.cpp
    cli/dispatch.cpp
    cli/options.cpp
    cli/session.cpp
  DEPS
    xdec_spec
    xdec_il
    xdec_binary
    xdec_pass
    xdec_passes
      xdec_decompile
      xdec_analysis
      xdec_emit
      xdec_plugin
      xdec_types
    xdec_support)

# The CLI loads the architecture spec from source, so it needs to know where the
# shipped specs live. An installed build would resolve this relative to the
# executable instead.
target_compile_definitions(xdec PRIVATE
  XDEC_SPEC_DIR="${CMAKE_SOURCE_DIR}/specs")

# Decompiled output defaults to `#include "xdec_helpers.h"` (see
# emit/c_printer.h). Copying it beside the built xdec.exe is what lets a user
# take generated .c straight to a compiler with no extra setup.
add_custom_command(TARGET xdec POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_SOURCE_DIR}/include/xdec/xdec_helpers.h"
    "$<TARGET_FILE_DIR:xdec>/xdec_helpers.h"
  COMMENT "Copying xdec_helpers.h next to xdec")
