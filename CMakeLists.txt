cmake_minimum_required(VERSION 3.20)
project(coglet C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

set(COMMON_SOURCES tools/ast/ast_print.c tests/file_utils_delete.h)

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
        include/utils/flag.h
)
target_include_directories(compiler_core PUBLIC include)

# --- The real compiler binary. ---
add_executable(coglet src/coglet.c ${COMMON_SOURCES})
target_link_libraries(coglet PRIVATE compiler_core)

add_executable(coglet_test_runner tools/tests/check_semantic_info.c ${COMMON_SOURCES})
target_link_libraries(coglet_test_runner PRIVATE compiler_core)

# --- Standalone Semantic checker -------------
add_executable(check_semantics tools/tests/check_semantics.c
        include/compiler_driver.h)
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
                COMMAND "$<TARGET_FILE:coglet_test_runner>" "${test_src}"
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
