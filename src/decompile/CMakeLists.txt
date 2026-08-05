# The orchestration layer: composes lifter, passes, and analyses into the
# discovery loop. Everything may depend on decompile; decompile depends on all
# of them, never the reverse.
xdec_add_library(xdec_decompile
  SOURCES
    driver.cpp
  DEPS
    xdec_analysis
    xdec_pass
    xdec_passes
    xdec_spec)
