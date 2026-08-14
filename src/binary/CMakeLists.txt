xdec_add_library(xdec_binary
  SOURCES
    backing_store.cpp
    dyld_cache/dyld_cache_loader.cpp
    dyld_cache/dyld_cache_metadata.cpp
    elf.cpp
    image.cpp
    macho.cpp
    memory_map.cpp
    target_profile.cpp
  DEPS
    xdec_support)

# Private headers (elf_format.h, field_reader.h) live next to the sources.
target_include_directories(xdec_binary PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
