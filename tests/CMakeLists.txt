# SPDX-License-Identifier: AGPL-3.0-only

set(CASI_UNIT_TESTS
    test_buf
    test_str
    test_fs
    test_paths
    test_config
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
