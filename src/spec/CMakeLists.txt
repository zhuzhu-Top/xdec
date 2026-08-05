xdec_add_library(xdec_spec
  SOURCES
    ast.cpp
    check.cpp
    compile.cpp
    encoding.cpp
    engine.cpp
    lexer.cpp
    lift.cpp
    parser.cpp
    program.cpp
    serialize.cpp
    symint.cpp
  DEPS
    xdec_il
    xdec_support)

# lexer.h is private to the front end.
target_include_directories(xdec_spec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
