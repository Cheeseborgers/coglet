# Host-C backend CTest helpers.
#
# These helpers keep host-C execution mechanics in one place so semantic
# execution fixtures can later be registered against another backend without
# duplicating CMake command plumbing. They intentionally remain host-C-specific
# until a second backend establishes the common interface worth abstracting.

function(add_host_c_executable_test name fixture expected_exit labels)
    set(test_command
            "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${fixture}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}
            -DEXPECT_EXIT=${expected_exit}
    )

    if(ARGC GREATER 4)
        list(APPEND test_command
                -DEXPECT_STDOUT_FILE=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${ARGV4}
        )
    endif()

    list(APPEND test_command
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
    )

    add_test(NAME ${name} COMMAND ${test_command})
    set_tests_properties(${name} PROPERTIES LABELS "${labels}")
endfunction()

function(add_host_c_library_test name fixture expected_exit style labels)
    add_test(
            NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${fixture}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}
            -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
            -DLIB_NAME=coglet_backend_link_support
            -DSTYLE=${style}
            -DEXPECT_EXIT=${expected_exit}
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
    )
    set_tests_properties(${name} PROPERTIES LABELS "${labels}")
endfunction()

function(add_host_c_trap_test name fixture no_trap_exit labels)
    add_test(
            NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${fixture}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}
            -DNO_TRAP_EXIT=${no_trap_exit}
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_trap_test.cmake"
    )
    set_tests_properties(${name} PROPERTIES LABELS "${labels}")
endfunction()

function(add_host_c_failure_test name fixture expected_exit expected_substring labels)
    add_test(
            NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${fixture}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}
            -DEXPECT_EXIT=${expected_exit}
            "-DEXPECT_SUBSTRING=${expected_substring}"
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_failure_test.cmake"
    )
    set_tests_properties(${name} PROPERTIES LABELS "${labels}")
endfunction()
