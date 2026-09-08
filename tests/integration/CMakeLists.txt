add_executable(test_reactor_integration
    test_reactor_integration.cpp
)

target_link_libraries(test_reactor_integration PRIVATE axiom_core)

set_target_properties(test_reactor_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ReactorIntegrationTest
    COMMAND test_reactor_integration
)

add_executable(test_signalfd_integration
    test_signalfd_integration.cpp
)

target_link_libraries(test_signalfd_integration PRIVATE axiom_core)

set_target_properties(test_signalfd_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME SignalfdIntegrationTest
    COMMAND test_signalfd_integration
)

add_executable(test_server_integration
    test_server_integration.cpp
)

target_link_libraries(test_server_integration PRIVATE axiom_core)

set_target_properties(test_server_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ServerIntegrationTest
    COMMAND test_server_integration
)

add_executable(test_process_integration
    test_process_integration.cpp
)

target_link_libraries(test_process_integration PRIVATE axiom_core)

set_target_properties(test_process_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ProcessIntegrationTest
    COMMAND test_process_integration
)

add_executable(test_server_exec_integration
    test_server_exec_integration.cpp
)

target_link_libraries(test_server_exec_integration PRIVATE axiom_core)

set_target_properties(test_server_exec_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ServerExecIntegrationTest
    COMMAND test_server_exec_integration
)

add_executable(test_ast_integration
    test_ast_integration.cpp
)

target_link_libraries(test_ast_integration PRIVATE axiom_core)

set_target_properties(test_ast_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME AstIntegrationTest
    COMMAND test_ast_integration
)

add_executable(test_server_ast_integration
    test_server_ast_integration.cpp
)

target_link_libraries(test_server_ast_integration PRIVATE axiom_core)

set_target_properties(test_server_ast_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ServerAstIntegrationTest
    COMMAND test_server_ast_integration
)

add_executable(test_cdp_integration
    test_cdp_integration.cpp
)

target_link_libraries(test_cdp_integration PRIVATE axiom_core)

set_target_properties(test_cdp_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME CdpIntegrationTest
    COMMAND test_cdp_integration
)

add_executable(test_server_cdp_integration
    test_server_cdp_integration.cpp
)

target_link_libraries(test_server_cdp_integration PRIVATE axiom_core)

set_target_properties(test_server_cdp_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
)

add_test(
    NAME ServerCdpIntegrationTest
    COMMAND test_server_cdp_integration
)

find_program(AXIOM_PYTHON_EXECUTABLE NAMES python3 python PATHS ${CMAKE_SOURCE_DIR}/.venv/bin NO_DEFAULT_PATH)
if(NOT AXIOM_PYTHON_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        set(AXIOM_PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
    endif()
endif()

if(AXIOM_PYTHON_EXECUTABLE)
    add_test(
        NAME FirecrackerE2ETest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_firecracker_e2e.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME ExecE2ETest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_exec_e2e.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME AstE2ETest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_ast_e2e.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME CdpE2ETest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_cdp_e2e.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME SnapshotE2ETest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_snapshot_e2e.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME McpIntegrationTest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_integration.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME AtebIntegrationTest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_ateb_benchmark.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(
        NAME CliIntegrationTest
        COMMAND ${AXIOM_PYTHON_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_integration.py -v
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    set_tests_properties(
        FirecrackerE2ETest
        ExecE2ETest
        AstE2ETest
        CdpE2ETest
        SnapshotE2ETest
        McpIntegrationTest
        AtebIntegrationTest
        CliIntegrationTest
        PROPERTIES TIMEOUT 60
    )
endif()

set_tests_properties(
    ReactorIntegrationTest
    SignalfdIntegrationTest
    ServerIntegrationTest
    ProcessIntegrationTest
    ServerExecIntegrationTest
    AstIntegrationTest
    ServerAstIntegrationTest
    CdpIntegrationTest
    ServerCdpIntegrationTest
    PROPERTIES TIMEOUT 30
)
