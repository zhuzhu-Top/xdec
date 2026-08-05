xdec_add_library(xdec_types
  SOURCES
    binder.cpp
    database.cpp
    hdecl_parser.cpp
    syscall_table.cpp
  DEPS
    xdec_support)

# Preset headers and the syscall table are shipped data, resolved by name
# ("android-ndk") as well as by path. An installed build would resolve this
# relative to the executable instead.
target_compile_definitions(xdec_types PRIVATE
  XDEC_TYPES_DIR="${CMAKE_SOURCE_DIR}/types")
