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
        src/source.c
        src/diagnostic.c
        src/lexer.c
        src/parser.c
        src/ast.c
        src/utils/utils.c
        src/utils/arena.c
        src/parser_diag.c
        src/semantic_anal.c
        src/target_info.c
        src/string_decode.c
        src/compiler_driver.c
        src/backend_c.c
        src/cog_ir.c
        src/cog_ir_verify.c
        src/cog_ir_dump.c
        src/cog_ir_lower.c
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

# --- Target description / cross-target semantic mapping checker. ---
add_executable(check_target_info tools/tests/check_target_info.c)
target_link_libraries(check_target_info PRIVATE compiler_core)

# --- Contextual-conversion semantic metadata checker. ---
add_executable(check_contextual_conversions tools/tests/check_contextual_conversions.c)
target_link_libraries(check_contextual_conversions PRIVATE compiler_core)

# --- Centralized semantic constant-value checker. ---
add_executable(check_constant_values tools/tests/check_constant_values.c)
target_link_libraries(check_constant_values PRIVATE compiler_core)

# --- Multi-file source provenance / diagnostic checker. ---
add_executable(check_source_provenance tools/tests/check_source_provenance.c)
target_link_libraries(check_source_provenance PRIVATE compiler_core)

add_test(
        NAME source_provenance_multi_file
        COMMAND "$<TARGET_FILE:check_source_provenance>"
)
set_tests_properties(
        source_provenance_multi_file
        PROPERTIES LABELS "source;diagnostic;multi-file"
)

# --- CogIR core builder/verifier/dump regression. ---
add_executable(check_cog_ir_core tools/tests/check_cog_ir_core.c)
target_link_libraries(check_cog_ir_core PRIVATE compiler_core)

add_test(
        NAME cog_ir_core_builder_verifier
        COMMAND "$<TARGET_FILE:check_cog_ir_core>"
)
set_tests_properties(
        cog_ir_core_builder_verifier
        PROPERTIES LABELS "ir;ir.core;verifier"
)

# --- Frontend semantic metadata -> CogIR mapping regression. ---
add_executable(check_cog_ir_lower_metadata tools/tests/check_cog_ir_lower_metadata.c)
target_link_libraries(check_cog_ir_lower_metadata PRIVATE compiler_core)
add_test(NAME cog_ir_frontend_metadata_lowering
        COMMAND "$<TARGET_FILE:check_cog_ir_lower_metadata>"
                "${CMAKE_SOURCE_DIR}/tests/test_assets/ir/metadata_lowering.cog")
set_tests_properties(cog_ir_frontend_metadata_lowering
        PROPERTIES LABELS "ir;ir.lowering;semantic;metadata")

# --- Standalone lexer test tool (Stage 1 harness). ---
add_executable(dump_tokens tools/dump_tokens.c ${COMMON_SOURCES})
target_link_libraries(dump_tokens PRIVATE compiler_core)

# --- Standalone parser test tool (Stage 2 harness). ---
add_executable(dump_ast tools/dump_ast.c ${COMMON_SOURCES})
target_link_libraries(dump_ast PRIVATE compiler_core)

# --- CogIR textual dump tool. ---
add_executable(dump_ir tools/dump_ir.c)
target_link_libraries(dump_ir PRIVATE compiler_core)

# --- Tests ---
enable_testing()

add_test(
        NAME cog_ir_executable_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/executable_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/executable_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_executable_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;cfg;module-init"
)

add_test(
        NAME cog_ir_structured_cfg_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/structured_cfg_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/structured_cfg_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_structured_cfg_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;cfg;control-flow;short-circuit;switch;module-init"
)

add_test(
        NAME cog_ir_data_address_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/data_address_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/data_address_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_data_address_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;data;aggregate;pointer;volatile;cast;string"
)

add_test(
        NAME cog_ir_wrapping_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/wrapping_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/wrapping_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_wrapping_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;wrapping;builtin"
)

add_test(
        NAME cog_ir_c_variadic_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/c_variadic_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/c_variadic_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_c_variadic_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;interop;abi;variadic"
)

add_test(
        NAME cog_ir_source_return_spelling_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/source_return_spelling.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/source_return_spelling.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_source_return_spelling_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;metadata;entry;abi-spelling"
)

add_test(
        NAME cog_ir_semantic_valid_fixture_suite
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DFIXTURE_DIR=${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/valid
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ir_valid_suite.cmake"
)
set_tests_properties(
        cog_ir_semantic_valid_fixture_suite
        PROPERTIES LABELS "ir;ir.lowering;ir.integration;semantic;semantic.valid"
)

add_test(
        NAME cog_ir_semantic_valid_integration
        COMMAND $<TARGET_FILE:dump_ir> ${CMAKE_SOURCE_DIR}/tests/test_assets/semantic_valid.cog
)
set_tests_properties(
        cog_ir_semantic_valid_integration
        PROPERTIES LABELS "ir;ir.lowering;ir.integration;semantic"
)

add_test(
        NAME target_info_cross_target_scalar_mapping
        COMMAND "$<TARGET_FILE:check_target_info>"
                "${CMAKE_SOURCE_DIR}/tests/test_assets/target/abi_scalar_mapping.cog"
)
set_tests_properties(
        target_info_cross_target_scalar_mapping
        PROPERTIES LABELS "target;semantic;interop;abi"
)

add_test(
        NAME semantic_contextual_conversion_metadata
        COMMAND "$<TARGET_FILE:check_contextual_conversions>"
                "${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/contextual_conversions.cog"
)
set_tests_properties(
        semantic_contextual_conversion_metadata
        PROPERTIES LABELS "semantic;semantic.info;conversion"
)

add_test(
        NAME semantic_constant_value_metadata
        COMMAND "$<TARGET_FILE:check_constant_values>"
                "${CMAKE_SOURCE_DIR}/tests/test_assets/semantic/constant_value_metadata.cog"
)
set_tests_properties(
        semantic_constant_value_metadata
        PROPERTIES LABELS "semantic;semantic.info;constant"
)

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
                    -DNORMALIZE_DIAGNOSTIC_PATH=ON
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
        NAME backend_checked_arithmetic
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/checked_arithmetic.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_checked_arithmetic
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=95
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_checked_arithmetic
        PROPERTIES LABELS "backend;backend.c;ir;checked-arithmetic"
)

add_test(
        NAME backend_structured_cfg
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/structured_cfg.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_structured_cfg
        -DEXPECT_EXIT=101
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_structured_cfg
        PROPERTIES LABELS "backend;backend.c;ir;cfg;module-init"
)

add_test(
        NAME backend_scalar_ops
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/scalar_ops.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_scalar_ops
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_scalar_ops
        PROPERTIES LABELS "backend;backend.c;ir;scalar;bitwise;float;pointer"
)

add_test(
        NAME backend_data_address_casts
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/data_address_casts.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_data_address_casts
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_data_address_casts
        PROPERTIES LABELS "backend;backend.c;ir;data;address;cast;volatile"
)

add_test(
        NAME backend_indirect_calls
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/indirect_calls.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_indirect_calls
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_indirect_calls
        PROPERTIES LABELS "backend;backend.c;ir;abi;callback;indirect-call;variadic"
)

add_test(
        NAME backend_aggregate_values
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/aggregate_values.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_aggregate_values
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_aggregate_values
        PROPERTIES LABELS "backend;backend.c;ir;aggregate;array;struct;global;interop"
)

foreach(cast_trap_case IN ITEMS integer float_integer float_narrow)
    add_test(
            NAME backend_checked_cast_${cast_trap_case}_trap
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/checked_cast_${cast_trap_case}_trap.cog
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_checked_cast_${cast_trap_case}_trap
            -DNO_TRAP_EXIT=91
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_trap_test.cmake"
    )
    set_tests_properties(
            backend_checked_cast_${cast_trap_case}_trap
            PROPERTIES LABELS "backend;backend.c;ir;cast;trap"
    )
endforeach()

add_test(
        NAME backend_shifts
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/shifts.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_shifts
        -DEXPECT_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_executable_test.cmake"
)
set_tests_properties(
        backend_shifts
        PROPERTIES LABELS "backend;backend.c;ir;shift;checked-count"
)

foreach(shift_trap_case IN ITEMS negative_count wide_count)
    add_test(
            NAME backend_shift_${shift_trap_case}_trap
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/shift_${shift_trap_case}_trap.cog
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_shift_${shift_trap_case}_trap
            -DNO_TRAP_EXIT=91
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_trap_test.cmake"
    )
    set_tests_properties(
            backend_shift_${shift_trap_case}_trap
            PROPERTIES LABELS "backend;backend.c;ir;shift;checked-count;trap"
    )
endforeach()

foreach(trap_case IN ITEMS overflow division_by_zero division_overflow)
    if(trap_case STREQUAL "overflow")
        set(trap_exit 96)
    elseif(trap_case STREQUAL "division_by_zero")
        set(trap_exit 97)
    else()
        set(trap_exit 98)
    endif()

    add_test(
            NAME backend_checked_${trap_case}_trap
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/checked_${trap_case}_trap.cog
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_checked_${trap_case}_trap
            -DNO_TRAP_EXIT=${trap_exit}
            -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_trap_test.cmake"
    )
    set_tests_properties(
            backend_checked_${trap_case}_trap
            PROPERTIES LABELS "backend;backend.c;ir;checked-arithmetic;trap"
    )
endforeach()

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
        NAME backend_wrapping_arithmetic
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/wrapping_arithmetic.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_wrapping_arithmetic
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=93
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_wrapping_arithmetic
        PROPERTIES LABELS "backend;backend.c;ir;wrapping"
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
        NAME backend_extern_c_callback
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_callback.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_callback
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=53
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_callback
        PROPERTIES LABELS "backend;backend.c;interop;abi;callback"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
add_test(
        NAME backend_extern_c_calling_convention
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_calling_convention.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_calling_convention
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=79
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_calling_convention
        PROPERTIES LABELS "backend;backend.c;interop;abi;calling-convention;callback"
)
endif()

add_test(
        NAME backend_rejects_nested_main_as_entry
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/nested_main_not_entry.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_nested_main_not_entry
        -DEXPECT_EXIT=3
        "-DEXPECT_SUBSTRING=requires a top-level Coglet 'main' function"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_failure_test.cmake"
)
set_tests_properties(
        backend_rejects_nested_main_as_entry
        PROPERTIES LABELS "backend;backend.c;backend.invalid;ir;entrypoint;nested-function"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND
   NOT WIN32)
add_test(
        NAME backend_rejects_unsupported_stdcall
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/unsupported_stdcall_calling_convention.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_unsupported_stdcall
        -DEXPECT_EXIT=3
        "-DEXPECT_SUBSTRING=call=stdcall requires 32-bit x86"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_failure_test.cmake"
)
set_tests_properties(
        backend_rejects_unsupported_stdcall
        PROPERTIES LABELS "backend;backend.c;backend.invalid;interop;abi;calling-convention"
)
endif()


add_test(
        NAME backend_extern_c_variadic
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_variadic.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_variadic
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=57
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_variadic
        PROPERTIES LABELS "backend;backend.c;interop;abi;variadic"
)

add_test(
        NAME backend_extern_c_incomplete_struct
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_incomplete_struct.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_incomplete_struct
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=67
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_incomplete_struct
        PROPERTIES LABELS "backend;backend.c;interop;abi;struct;incomplete"
)

add_test(
        NAME backend_extern_c_repr_union
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_union.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_union
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=71
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_union
        PROPERTIES LABELS "backend;backend.c;interop;abi;union;struct;array"
)


if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
add_test(
        NAME backend_extern_c_repr_layout
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_repr_layout.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_repr_layout
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=73
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_repr_layout
        PROPERTIES LABELS "backend;backend.c;interop;abi;layout;packed;alignment"
)
endif()

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

add_test(
        NAME backend_extern_c_volatile_pointers
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/extern_c_volatile_pointers.cog
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/backend_extern_c_volatile_pointers
        -DLIB_DIR=$<TARGET_FILE_DIR:coglet_backend_link_support>
        -DLIB_NAME=coglet_backend_link_support
        -DSTYLE=joined
        -DEXPECT_EXIT=89
        -P "${CMAKE_SOURCE_DIR}/cmake/run_backend_library_test.cmake"
)
set_tests_properties(
        backend_extern_c_volatile_pointers
        PROPERTIES LABELS "backend;backend.c;interop;abi;volatile"
)
