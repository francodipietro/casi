# SPDX-License-Identifier: AGPL-3.0-only

set(CASI_UNIT_TESTS
    test_buf
    test_str
    test_fs
    test_paths
    test_config
    test_jsonl
    test_shared_config
    test_index
    test_encoding
    test_roots
    test_chunk
    test_crypto
)

foreach(test_name IN LISTS CASI_UNIT_TESTS)
    add_executable(${test_name} unit/${test_name}.c)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${test_name} PRIVATE casi_core)
    target_compile_options(${test_name} PRIVATE ${CASI_WARN_FLAGS})
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The CLI has to behave as a process, not just as linked code: exit statuses
# and the stdout/stderr split are part of the contract.
add_test(NAME cli_version COMMAND casi --version)
set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "casi ${PROJECT_VERSION}")

add_test(NAME cli_help COMMAND casi --help)
set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: casi <command>")

add_test(NAME cli_unknown_command COMMAND casi definitely-not-a-command)
set_tests_properties(cli_unknown_command PROPERTIES WILL_FAIL TRUE)

# A failing command prints one error line, never the "unknown error" placeholder.
add_test(NAME cli_errors
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cli_errors.sh $<TARGET_FILE:casi>)

# End-to-end, against a real file:// remote and no network: two machines with
# different local layouts, and the conflict path between them.
set(CASI_INTEGRATION_TESTS
    two_machines
    conflict
    init
    shared_config
    doctor
    auxiliary_files
    nested_roots
    format
    encryption
    diagnose
    large_session
    gc
    interruption
)
foreach(test_name IN LISTS CASI_INTEGRATION_TESTS)
    add_test(NAME ${test_name}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/integration/${test_name}.sh
                $<TARGET_FILE:casi> ${CMAKE_CURRENT_BINARY_DIR}/tmp/${test_name})
endforeach()
