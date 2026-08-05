xdec_add_executable(xdec
  SOURCES
    xdec_main.cpp
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
