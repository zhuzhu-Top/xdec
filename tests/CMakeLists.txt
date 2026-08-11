include(FetchContent)

# Prefer an installed Catch2 so offline builds work; fall back to fetching it.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  message(STATUS "Catch2 not found locally; fetching v3.7.1")
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)
endif()

list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(xdec_tests
  support/test_bits.cpp
  support/test_arena.cpp
  support/test_handle.cpp
  support/test_result.cpp
  analysis/test_analysis.cpp
  analysis/test_dispatcher_shape.cpp
  analysis/test_emit_redundancy.cpp
  analysis/test_expr_reuse.cpp
  analysis/test_image_eval.cpp
  analysis/test_import_callee.cpp
  analysis/test_index_bound.cpp
  analysis/test_jump_table.cpp
  analysis/test_live_register_frame.cpp
  analysis/test_load_inline.cpp
  analysis/test_plt_stub.cpp
  analysis/test_profile.cpp
  analysis/test_stack_frame.cpp
  analysis/test_stack_load_fold.cpp
  analysis/test_stack_store_fold.cpp
  analysis/test_typed_variables.cpp
  analysis/test_variables.cpp
  decompile/test_driver.cpp
  emit/test_structure.cpp
  emit/test_c_printer.cpp
  emit/test_c_atomic.cpp
  emit/test_c_expr.cpp
  emit/test_c_expr_reuse.cpp
  emit/test_c_helpers_header.cpp
  emit/test_c_load_inline.cpp
  emit/test_c_stack_load_inline.cpp
    emit/test_c_import_call.cpp
    emit/test_c_syscall.cpp
    emit/test_c_types.cpp
  emit/test_c_phi_edges.cpp
  emit/test_c_typed_return.cpp
  binary/test_memory_map.cpp
  binary/test_elf.cpp
  il/test_type.cpp
  il/test_expr.cpp
  il/test_function.cpp
  il/test_interp.cpp
  il/test_roundtrip.cpp
  il/test_verify.cpp
  corpus/test_corpus.cpp
  pass/test_pass.cpp
  pass/test_observe.cpp
    passes/test_cfg_finalize.cpp
    passes/test_const_fold_memory.cpp
    passes/test_fold_resolved_branch.cpp
    passes/test_local_simplify.cpp
    passes/test_apply_types.cpp
    passes/test_recover_syscall.cpp
    passes/test_recover_tailcall.cpp
    passes/test_resolve_call.cpp
    passes/test_resolve_indirect.cpp
    passes/test_algebra.cpp
    passes/test_ssa_construct.cpp
    passes/test_ssa_optimize.cpp
    passes/test_stack_prop.cpp
    passes/test_trampoline_fold.cpp
    passes/test_vars.cpp
  plugin/test_plugin.cpp
  spec/test_lift_function.cpp
  spec/test_spec_lexer.cpp
  spec/test_spec_symint.cpp
  spec/test_spec_parser.cpp
  spec/test_spec_check.cpp
  spec/test_spec_encoding.cpp
  spec/test_spec_engine.cpp
  types/test_type_database.cpp
  types/test_hdecl_parser.cpp
  types/test_syscall_table.cpp
  types/test_binder.cpp)

# The spec tests read the shipped .xspec files rather than a copy of them, so
# that a spec which stops checking cannot pass the suite.
target_compile_definitions(xdec_tests PRIVATE
  XDEC_SPEC_DIR="${PROJECT_SOURCE_DIR}/specs")

# Real plugins for the loader tests: one good, one reporting a stale ABI
# version, one whose init throws across the boundary.
add_library(xdec_test_plugin_echo SHARED plugin/echo_plugin.cpp)
add_library(xdec_test_plugin_old_abi SHARED plugin/echo_plugin.cpp)
add_library(xdec_test_plugin_thrower SHARED plugin/thrower_plugin.cpp)
target_compile_definitions(xdec_test_plugin_old_abi PRIVATE XDEC_PLUGIN_FAKE_VERSION=999)
foreach(plugin xdec_test_plugin_echo xdec_test_plugin_old_abi xdec_test_plugin_thrower)
  target_link_libraries(${plugin} PRIVATE xdec_pass xdec_il xdec_support xdec_warnings xdec_options)
  set_target_properties(${plugin} PROPERTIES PREFIX "")
endforeach()

target_compile_definitions(xdec_tests PRIVATE
  XDEC_TEST_PLUGIN_ECHO="$<TARGET_FILE:xdec_test_plugin_echo>"
  XDEC_TEST_PLUGIN_OLD_ABI="$<TARGET_FILE:xdec_test_plugin_old_abi>"
  XDEC_TEST_PLUGIN_THROWER="$<TARGET_FILE:xdec_test_plugin_thrower>")

# The loader tests need the plugin libraries built before they run.
add_dependencies(xdec_tests
  xdec_test_plugin_echo
  xdec_test_plugin_old_abi
  xdec_test_plugin_thrower)

target_include_directories(xdec_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(xdec_tests PRIVATE
  xdec_analysis
  xdec_binary
  xdec_decompile
  xdec_emit
  xdec_il
  xdec_pass
  xdec_passes
  xdec_plugin
  xdec_spec
  xdec_types
  xdec_support
  xdec_warnings
  xdec_options
  Catch2::Catch2WithMain)

catch_discover_tests(xdec_tests)

