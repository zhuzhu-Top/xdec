find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

if(NOT pybind11_DIR)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE _xdec_pybind11_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _xdec_pybind11_rc)
  if(_xdec_pybind11_rc EQUAL 0 AND EXISTS "${_xdec_pybind11_dir}")
    set(pybind11_DIR "${_xdec_pybind11_dir}")
  endif()
endif()

find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(pyxdec MODULE pyxdec.cpp)
set_target_properties(pyxdec PROPERTIES
  OUTPUT_NAME "xdec"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

target_include_directories(pyxdec PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_compile_definitions(pyxdec PRIVATE
  XDEC_SPEC_DIR="${CMAKE_SOURCE_DIR}/specs")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(pyxdec PRIVATE MS_WIN64)
endif()
if(MINGW)
  target_link_options(pyxdec PRIVATE -static)
endif()

target_link_libraries(pyxdec PRIVATE
  xdec_options
  xdec_decompile
  xdec_devirt
  xdec_analysis
  xdec_binary
  xdec_exec
  xdec_emit
  xdec_passes
  xdec_pass
  xdec_il
  xdec_spec
  xdec_storage
  xdec_types
  xdec_support
  xdec_obs)
