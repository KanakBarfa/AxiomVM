add_executable(axiom-initd
    src/main.cpp
)

target_link_libraries(axiom-initd PRIVATE axiom_core)

set_target_properties(axiom-initd PROPERTIES
    OUTPUT_NAME "axiom-initd"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
