cmake_minimum_required(VERSION 3.25)
project(
    AxiomVM
    VERSION 0.1.0
    DESCRIPTION "The Token-First MicroVM Appliance for AI Agents"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AXIOM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(AXIOM_STATIC_BUILD "Build hermetic static binaries" OFF)
option(AXIOM_BUILD_TESTS "Build integration and end-to-end test suites" ON)

add_library(axiom_compiler_flags INTERFACE)
target_compile_features(axiom_compiler_flags INTERFACE cxx_std_23)

target_compile_options(axiom_compiler_flags INTERFACE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
)

if(AXIOM_WARNINGS_AS_ERRORS)
    target_compile_options(axiom_compiler_flags INTERFACE -Werror)
endif()

if(AXIOM_STATIC_BUILD)
    target_link_options(axiom_compiler_flags INTERFACE -static)
endif()

add_library(axiom_core INTERFACE)
target_include_directories(axiom_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(axiom_core INTERFACE axiom_compiler_flags)

set(GRAMMAR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/grammars")
if(EXISTS "${GRAMMAR_DIR}/c/parser.c")
    set(TS_GRAMMAR_SOURCES
        "${GRAMMAR_DIR}/c/parser.c"
        "${GRAMMAR_DIR}/cpp/parser.c"
        "${GRAMMAR_DIR}/cpp/scanner.c"
        "${GRAMMAR_DIR}/go/parser.c"
        "${GRAMMAR_DIR}/rust/parser.c"
        "${GRAMMAR_DIR}/rust/scanner.c"
    )

    add_library(axiom_grammars STATIC ${TS_GRAMMAR_SOURCES})
    set_target_properties(axiom_grammars PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    target_include_directories(axiom_grammars PRIVATE
        "${GRAMMAR_DIR}/c"
        "${GRAMMAR_DIR}/cpp"
        "${GRAMMAR_DIR}/go"
        "${GRAMMAR_DIR}/rust"
    )
    target_compile_options(axiom_grammars PRIVATE -w -O2)
    target_link_libraries(axiom_grammars PUBLIC tree-sitter)
    target_link_libraries(axiom_core INTERFACE axiom_grammars)
endif()

add_subdirectory(guest/axiom-initd)

if(AXIOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
