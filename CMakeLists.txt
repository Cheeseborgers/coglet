cmake_minimum_required(VERSION 3.20)
project(coglet C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

set(COMMON_SOURCES tools/ast/ast_print.c)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message("found cc")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif()

# --- Core compiler logic, shared by every tool/binary. ---
# Not all of this need to be shared tho
add_library(compiler_core STATIC
        src/lexer.c
        src/parser.c
        src/ast.c
        src/utils/utils.c
        src/utils/arena.c
        src/parser_diag.c
        src/semantic_anal.c
        src/string_decode.c
        src/compiler_driver.c
        src/backend_c.c
)
target_include_directories(compiler_core PUBLIC include)

# --- The real compiler binary. ---
add_executable(coglet src/coglet.c ${COMMON_SOURCES})
target_link_libraries(coglet PRIVATE compiler_core)

# --- Standalone Semantic info checker -------------
add_executable(check_semantic_info
        tools/tests/check_semantic_info.c
        tools/tests/semantic_info_verifier.c
        tools/tests/semantic_info_verifier.h
        ${COMMON_SOURCES})
target_link_libraries(check_semantic_info PRIVATE compiler_core)

# --- Standalone Semantic checker -------------
add_executable(check_semantics tools/tests/check_semantics.c)
target_link_libraries(check_semantics PRIVATE compiler_core)

# --- Standalone lexer test tool (Stage 1 harness). ---
add_executable(dump_tokens tools/dump_tokens.c ${COMMON_SOURCES})
target_link_libraries(dump_tokens PRIVATE compiler_core)

# --- Standalone parser test tool (Stage 2 harness). ---
add_executable(dump_ast tools/dump_ast.c ${COMMON_SOURCES})
target_link_libraries(dump_ast PRIVATE compiler_core)

# --- Tests ---
enable_testing()

# Registers one ctest per <name>.cog / <name>.expected pair found in
# `dir`, running `exe_target` on the .cog and diffing stdout against
# the .expected file. Reused for both tests/lexer and tests/parser so
# adding a new test stage later is just one more call to this.
function(add_diff_tests prefix dir exe_target)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
            "${dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        file(RELATIVE_PATH relative_path "${dir}" "${test_src}")
        get_filename_component(relative_dir "${relative_path}" DIRECTORY)
        get_filename_component(test_name "${test_src}" NAME_WE)

        # Invalid parser fixtures have their own harness because dump_ast
        # exits non-zero and writes diagnostics to stderr.
        if(prefix STREQUAL "parser" AND relative_dir MATCHES "^invalid([/\\]|$)")
            continue()
        endif()

        set(expected_file "${test_src}")
        string(REGEX REPLACE "\\.cog$" ".expected" expected_file "${expected_file}")

        if(relative_dir STREQUAL "")
            set(ctest_name "${prefix}_${test_name}")
        else()
            string(REPLACE "/" "_" category_name "${relative_dir}")
            string(REPLACE "\\" "_" category_name "${category_name}")
            set(ctest_name "${prefix}_${category_name}_${test_name}")
        endif()

        if(EXISTS "${expected_file}")
            add_test(
                    NAME "${ctest_name}"
                    COMMAND "${CMAKE_COMMAND}"
                    -DEXE=$<TARGET_FILE:${exe_target}>
                    -DINPUT=${test_src}
                    -DEXPECTED=${expected_file}
                    -DEXPECT_EXIT=0
                    -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
            )

            set_tests_properties(
                    "${ctest_name}"
                    PROPERTIES
                    LABELS "${prefix};snapshot"
            )
        else()
            message(WARNING
                    "No .expected file for ${test_src}; skipping test"
            )
        endif()
    endforeach()
endfunction()


function(add_invalid_parser_tests root_dir)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
            "${root_dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        file(RELATIVE_PATH relative_path "${root_dir}" "${test_src}")
        get_filename_component(relative_dir "${relative_path}" DIRECTORY)
        get_filename_component(test_name "${test_src}" NAME_WE)

        set(expected_file "${test_src}")
        string(REGEX REPLACE "\\.cog$" ".expected" expected_file "${expected_file}")

        if(relative_dir STREQUAL "")
            set(ctest_name "parser_invalid_${test_name}")
            set(category_label "parser.invalid")
        else()
            string(REPLACE "/" "_" category_name "${relative_dir}")
            string(REPLACE "\\" "_" category_name "${category_name}")
            set(ctest_name "parser_invalid_${category_name}_${test_name}")

            string(REPLACE "/" "." category_label "${relative_dir}")
            string(REPLACE "\\" "." category_label "${category_label}")
            set(category_label "parser.invalid.${category_label}")
        endif()

        if(EXISTS "${expected_file}")
            add_test(
                    NAME "${ctest_name}"
                    COMMAND "${CMAKE_COMMAND}"
                    -DEXE=$<TARGET_FILE:dump_ast>
                    -DINPUT=${test_src}
                    -DEXPECTED=${expected_file}
                    -P "${CMAKE_SOURCE_DIR}/cmake/run_parser_invalid_test.cmake"
            )

            set_tests_properties(
                    "${ctest_name}"
                    PROPERTIES
                    LABELS "parser;parser.invalid;${category_label}"
            )
        else()
            message(WARNING
                    "No .expected file for ${test_src}; skipping parser invalid test"
            )
        endif()
    endforeach()
endfunction()

function(add_semantic_valid_tests root_dir)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
            "${root_dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        file(RELATIVE_PATH relative_path "${root_dir}" "${test_src}")
        get_filename_component(relative_dir "${relative_path}" DIRECTORY)
        get_filename_component(test_name "${test_src}" NAME_WE)

        if(relative_dir STREQUAL "")
            set(ctest_name "semantic_valid_${test_name}")
            set(category_label "semantic.uncategorized")
        else()
            string(REPLACE "/" "_" category_name "${relative_dir}")
            string(REPLACE "\\" "_" category_name "${category_name}")
            set(ctest_name "semantic_valid_${category_name}_${test_name}")

            string(REPLACE "/" "." category_label "${relative_dir}")
            string(REPLACE "\\" "." category_label "${category_label}")
            set(category_label "semantic.${category_label}")
        endif()

        add_test(
                NAME "${ctest_name}"
                COMMAND "$<TARGET_FILE:check_semantics>" "${test_src}"
        )

        set_tests_properties(
                "${ctest_name}"
                PROPERTIES
                LABELS "semantic;semantic.valid;${category_label}"
        )
    endforeach()
endfunction()

function(add_semantic_invalid_tests root_dir)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
            "${root_dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        file(RELATIVE_PATH relative_path "${root_dir}" "${test_src}")
        get_filename_component(relative_dir "${relative_path}" DIRECTORY)
        get_filename_component(test_name "${test_src}" NAME_WE)

        set(expected_file "${test_src}")
        string(REGEX REPLACE "\\.cog$" ".expected" expected_file "${expected_file}")

        if(relative_dir STREQUAL "")
            set(ctest_name "semantic_invalid_${test_name}")
            set(category_label "semantic.uncategorized")
        else()
            string(REPLACE "/" "_" category_name "${relative_dir}")
            string(REPLACE "\\" "_" category_name "${category_name}")
            set(ctest_name "semantic_invalid_${category_name}_${test_name}")

            string(REPLACE "/" "." category_label "${relative_dir}")
            string(REPLACE "\\" "." category_label "${category_label}")
            set(category_label "semantic.${category_label}")
        endif()

        if(EXISTS "${expected_file}")
            add_test(
                    NAME "${ctest_name}"
                    COMMAND "${CMAKE_COMMAND}"
                    -DEXE=$<TARGET_FILE:check_semantics>
                    -DINPUT=${test_src}
                    -DEXPECTED=${expected_file}
                    -DEXPECT_EXIT=1
                    -DOUTPUT_STREAM=stderr
                    -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
            )

            set_tests_properties(
                    "${ctest_name}"
                    PROPERTIES
                    LABELS "semantic;semantic.invalid;${category_label}"
            )
        else()
            message(WARNING
                    "No .expected file for ${test_src}; skipping semantic invalid test"
            )
        endif()
    endforeach()
endfunction()

function(add_semantic_info_tests root_dir)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
            "${root_dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        file(RELATIVE_PATH relative_path "${root_dir}" "${test_src}")
        get_filename_component(relative_dir "${relative_path}" DIRECTORY)
        get_filename_component(test_name "${test_src}" NAME_WE)

        if(relative_dir STREQUAL "")
            set(ctest_name "semantic_info_${test_name}")
            set(category_label "semantic.uncategorized")
        else()
            string(REPLACE "/" "_" category_name "${relative_dir}")
            string(REPLACE "\\" "_" category_name "${category_name}")
            set(ctest_name "semantic_info_${category_name}_${test_name}")

            string(REPLACE "/" "." category_label "${relative_dir}")
            string(REPLACE "\\" "." category_label "${category_label}")
            set(category_label "semantic.${category_label}")
        endif()

        add_test(
                NAME "${ctest_name}"
                COMMAND "$<TARGET_FILE:check_semantic_info>" "${test_src}"
        )

        set_tests_properties(
                "${ctest_name}"
                PROPERTIES
                LABELS "semantic;semantic.info;${category_label}"
        )
    endforeach()
endfunction()

function(add_invalid_lexer_tests root_dir)
    file(
            GLOB test_sources
            CONFIGURE_DEPENDS
            "${root_dir}/*.cog"
    )

    foreach(test_src IN LISTS test_sources)
        get_filename_component(test_name "${test_src}" NAME_WE)

        set(expected_stdout
                "${root_dir}/${test_name}.stdout.expected"
        )

        set(expected_stderr
                "${root_dir}/${test_name}.stderr.expected"
        )

        if(NOT EXISTS "${expected_stdout}")
            message(FATAL_ERROR
                    "Missing expected stdout file: ${expected_stdout}"
            )
        endif()

        if(NOT EXISTS "${expected_stderr}")
            message(FATAL_ERROR
                    "Missing expected stderr file: ${expected_stderr}"
            )
        endif()

        add_test(
                NAME "lexer_invalid_${test_name}"
                COMMAND
                "${CMAKE_COMMAND}"
                "-DEXE=$<TARGET_FILE:dump_tokens>"
                "-DINPUT=${test_src}"
                "-DEXPECTED_STDOUT=${expected_stdout}"
                "-DEXPECTED_STDERR=${expected_stderr}"
                "-DEXPECT_EXIT=1"
                -P "${CMAKE_SOURCE_DIR}/cmake/run_output_test.cmake"
        )

        set_tests_properties(
                "lexer_invalid_${test_name}"
                PROPERTIES
                LABELS "lexer;lexer.invalid"
        )
    endforeach()
endfunction()

# lexer tests
add_diff_tests(lexer  ${CMAKE_SOURCE_DIR}/tests/test_assets/lexer/valid  dump_tokens)
add_invalid_lexer_tests(${CMAKE_SOURCE_DIR}/tests/test_assets/lexer/invalid)

# parser tests
add_diff_tests(parser ${CMAKE_SOURCE_DIR}/tests/test_assets/parser dump_ast)
add_invalid_parser_tests(${CMAKE_SOURCE_DIR}/tests/test_assets/parser/invalid)

# semantic anal tests
add_semantic_valid_tests(${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/valid)
add_semantic_invalid_tests(${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/invalid)
add_semantic_info_tests(${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/valid)

# Copy assets
add_custom_command(
        TARGET coglet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/tests/test_assets
        $<TARGET_FILE_DIR:coglet>/test_assets
)

# Initial host-C backend integration tests. These compile Coglet source to a
# native executable through `cc`, then run it. The chosen libc function uses
# only c_int so this stage does not depend on string lowering yet.
add_test(
        NAME backend_extern_c_default_symbol
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_default_symbol.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_default_symbol
        -DEXPECT_EXIT=5
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_extern_c_default_symbol
        PROPERTIES LABELS "backend;backend.c;interop"
)

add_test(
        NAME backend_extern_c_symbol_override
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_symbol_override.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_symbol_override
        -DEXPECT_EXIT=7
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_extern_c_symbol_override
        PROPERTIES LABELS "backend;backend.c;interop"
)

add_test(
        NAME backend_rejects_unlowered_checked_binary
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/unsupported_checked_binary.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_unsupported_checked_binary
        -DEXPECT_EXIT=3
        "-DEXPECT_SUBSTRING=runtime binary operations are not lowered yet"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_failure_test.cmake"
)
set_tests_properties(
        backend_rejects_unlowered_checked_binary
        PROPERTIES LABELS "backend;backend.c;backend.invalid"
)

add_test(
        NAME backend_extern_c_pointer_types
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_pointer_types.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_pointer_types
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_extern_c_pointer_types
        PROPERTIES LABELS "backend;backend.c;interop"
)

add_test(
        NAME backend_extern_c_string_literal
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_string_literal.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_string_literal
        -DEXPECT_EXIT=0
        -DEXPECT_STDOUT_FILE=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_string_literal.stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_extern_c_string_literal
        PROPERTIES LABELS "backend;backend.c;interop;strings"
)

# Explicit library/linker selection for the host-C executable backend. The
# support library is intentionally outside the default link set so these tests
# prove that -L/-l forwarding is doing real work.
add_library(coglet_backend_link_support STATIC
        tests/support/backend_link_support.c
)

add_test(
        NAME backend_explicit_library_split_flags
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_explicit_library.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_explicit_library_split_flags
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=split
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_explicit_library_split_flags
        PROPERTIES LABELS "backend;backend.c;interop;linker"
)

add_test(
        NAME backend_explicit_library_joined_flags
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_explicit_library.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_explicit_library_joined_flags
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_explicit_library_joined_flags
        PROPERTIES LABELS "backend;backend.c;interop;linker"
)

add_test(
        NAME backend_extern_c_integer_aliases
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_integer_aliases.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_integer_aliases
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=29
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_integer_aliases
        PROPERTIES LABELS "backend;backend.c;interop;abi"
)

add_test(
        NAME backend_extern_c_scalar_aliases
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_scalar_aliases.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_scalar_aliases
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=31
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_scalar_aliases
        PROPERTIES LABELS "backend;backend.c;interop;abi"
)

add_test(
        NAME backend_extern_c_repr_struct
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_struct.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_struct
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=37
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_struct
        PROPERTIES LABELS "backend;backend.c;interop;abi;struct"
)

add_test(
        NAME backend_extern_c_repr_nested_struct
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_nested_struct.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_nested_struct
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=41
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_nested_struct
        PROPERTIES LABELS "backend;backend.c;interop;abi;struct"
)

add_test(
        NAME backend_extern_c_repr_array_fields
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_array_fields.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_array_fields
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=45
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_array_fields
        PROPERTIES LABELS "backend;backend.c;interop;abi;struct;array"
)

add_test(
        NAME backend_extern_c_repr_enum
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_enum.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_enum
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=49
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_enum
        PROPERTIES LABELS "backend;backend.c;interop;abi;enum;struct"
)

add_test(
        NAME driver_link_options_require_output
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_explicit_library.cog
        -DEXPECT_EXIT=3
        "-DEXPECT_SUBSTRING=linker options require -o <executable>"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_driver_failure_test.cmake"
)
set_tests_properties(
        driver_link_options_require_output
        PROPERTIES LABELS "driver;backend;linker;invalid"
)
