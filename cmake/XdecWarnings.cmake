# A shared warning set. Decompiler code does a lot of bit manipulation and
# integer narrowing, so conversion warnings are kept on deliberately: a silent
# truncation in a decoder is a correctness bug, not a style issue.

add_library(xdec_warnings INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xdec_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnon-virtual-dtor
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wextra-semi
    -Wmisleading-indentation
    -Wformat=2)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(xdec_warnings INTERFACE
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op)
  endif()
elseif(MSVC)
  target_compile_options(xdec_warnings INTERFACE /W4 /permissive- /Zc:preprocessor)
endif()

if(XDEC_WERROR)
  if(MSVC)
    target_compile_options(xdec_warnings INTERFACE /WX)
  else()
    target_compile_options(xdec_warnings INTERFACE -Werror)
  endif()
endif()

# Convenience wrapper: every xdec target links these two interface targets.
function(xdec_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${ARG_SOURCES})
  target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(${name} PUBLIC ${ARG_DEPS} PRIVATE xdec_warnings xdec_options)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(xdec_add_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(${name} PRIVATE ${ARG_DEPS} xdec_warnings xdec_options)
endfunction()
