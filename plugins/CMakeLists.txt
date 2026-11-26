# Bundled optional decompilation plugins (specialized for OLLVM / Mach-O ObjC+libc++).
# Loaded at runtime via `xdec decompile ... --plugin <path>`.

add_library(xdec_plugin_objc_libcxx SHARED objc_libcxx.cpp)
target_link_libraries(xdec_plugin_objc_libcxx PRIVATE
  xdec_analysis
  xdec_pass
  xdec_il
  xdec_support
  xdec_warnings
  xdec_options)
set_target_properties(xdec_plugin_objc_libcxx PROPERTIES
  PREFIX ""
  OUTPUT_NAME "objc_libcxx"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

add_library(xdec_plugin_ollvm_deobf SHARED ollvm_deobf.cpp)
target_link_libraries(xdec_plugin_ollvm_deobf PRIVATE
  xdec_analysis
  xdec_pass
  xdec_il
  xdec_support
  xdec_warnings
  xdec_options)
set_target_properties(xdec_plugin_ollvm_deobf PROPERTIES
  PREFIX ""
  OUTPUT_NAME "ollvm_deobf"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
