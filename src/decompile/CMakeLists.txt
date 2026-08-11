# The orchestration layer: composes lifter, passes, and analyses into the
# discovery loop. Everything may depend on decompile; decompile depends on all
# of them, never the reverse.
#
# xdec_emit is the one exception to "never the reverse" in spirit only: emit.h
# (decompileToC) is Vars-IL-to-C, layered on top of driver.cpp's IL-only
# decompile(), and xdec_emit itself has no dependency back on xdec_decompile,
# so the two-way rule the comment above states still holds.
xdec_add_library(xdec_decompile
  SOURCES
    driver.cpp
    emit.cpp
  DEPS
    xdec_analysis
    xdec_emit
    xdec_pass
    xdec_passes
    xdec_spec)
