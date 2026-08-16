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

# --- Frontend, semantic analysis, and CogIR shared by compiler tools/backends. ---
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
        src/cog_ir.c
        src/cog_ir_verify.c
        src/cog_ir_dump.c
        src/cog_ir_lower.c
)
target_include_directories(compiler_core PUBLIC include)

# --- Host-C backend. Backends depend on CogIR/core, never the reverse. ---
add_library(coglet_backend_c STATIC
        src/backend_c.c
)
target_include_directories(coglet_backend_c PUBLIC include)
target_link_libraries(coglet_backend_c PRIVATE compiler_core)

# --- Optional LLVM backend. ---
set(COGLET_LLVM "AUTO" CACHE STRING "LLVM backend mode: AUTO, ON, or OFF")
set_property(CACHE COGLET_LLVM PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${COGLET_LLVM}" COGLET_LLVM_MODE)
if(NOT COGLET_LLVM_MODE MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "COGLET_LLVM must be AUTO, ON, or OFF")
endif()

set(COGLET_HAS_LLVM_BACKEND OFF)
if(NOT COGLET_LLVM_MODE STREQUAL "OFF")
    # Find the installed LLVM package without a requested package version.
    # LLVM's ConfigVersion file may intentionally reject cross-major requests,
    # so `find_package(LLVM 17 ...)` can reject LLVM 18+ even though Coglet's
    # backend supports LLVM 17 and newer. Enforce our minimum explicitly after
    # loading LLVMConfig.cmake instead.
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND AND LLVM_PACKAGE_VERSION VERSION_LESS 17)
        if(COGLET_LLVM_MODE STREQUAL "ON")
            message(FATAL_ERROR
                "COGLET_LLVM=ON requires LLVM 17 or newer, but LLVM "
                "${LLVM_PACKAGE_VERSION} was found at ${LLVM_DIR}."
            )
        else()
            message(STATUS
                "Coglet LLVM backend: disabled (LLVM ${LLVM_PACKAGE_VERSION} is older than 17)"
            )
        endif()
    elseif(LLVM_FOUND)
        message(STATUS "Coglet LLVM backend: LLVM ${LLVM_PACKAGE_VERSION}")
        # Prefer LLVM's monolithic shared-library target when the installed
        # package provides it. Distribution builds commonly enable
        # LLVM_LINK_LLVM_DYLIB and do not install per-component libraries such
        # as LLVMCore/LLVMAnalysis. LLVM's own CMake helpers make the same
        # distinction instead of unconditionally mapping component names.
        if(TARGET LLVM)
            set(COGLET_LLVM_LIBS LLVM)
        elseif(LLVM_LINK_LLVM_DYLIB)
            find_library(COGLET_LLVM_DYLIB_LIBRARY
                NAMES LLVM LLVM-${LLVM_VERSION_MAJOR}
                HINTS ${LLVM_LIBRARY_DIRS}
            )
            if(NOT COGLET_LLVM_DYLIB_LIBRARY)
                message(FATAL_ERROR
                    "LLVM ${LLVM_PACKAGE_VERSION} requests monolithic linking, "
                    "but libLLVM could not be found in LLVM_LIBRARY_DIRS."
                )
            endif()
            set(COGLET_LLVM_LIBS "${COGLET_LLVM_DYLIB_LIBRARY}")
        else()
            llvm_map_components_to_libnames(COGLET_LLVM_LIBS core analysis support target native)
        endif()
        separate_arguments(COGLET_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

        add_library(coglet_backend_llvm STATIC
                src/backends/llvm/backend_llvm.c
                src/backends/llvm/lower_types.c
                src/backends/llvm/lower_integer.c
                src/backends/llvm/lower_float.c
                src/backends/llvm/lower_memory.c
                src/backends/llvm/lower_control.c
                src/backends/llvm/target.c
        )
        target_include_directories(coglet_backend_llvm
                PUBLIC include
                PRIVATE ${LLVM_INCLUDE_DIRS}
        )
        target_compile_options(coglet_backend_llvm PRIVATE ${COGLET_LLVM_DEFINITIONS})
        target_link_libraries(coglet_backend_llvm PRIVATE compiler_core ${COGLET_LLVM_LIBS})

        add_executable(check_llvm_stage4_trap tools/tests/check_llvm_stage4_trap.c)
        target_link_libraries(check_llvm_stage4_trap PRIVATE compiler_core coglet_backend_llvm)

        set(COGLET_HAS_LLVM_BACKEND ON)
    elseif(COGLET_LLVM_MODE STREQUAL "ON")
        message(FATAL_ERROR
            "COGLET_LLVM=ON but LLVMConfig.cmake was not found. "
            "Install an LLVM 17+ development package or set LLVM_DIR."
        )
    else()
        message(STATUS "Coglet LLVM backend: disabled (LLVM development package not found)")
    endif()
endif()

# --- The real compiler binary. ---
add_executable(coglet src/coglet.c ${COMMON_SOURCES})
target_link_libraries(coglet PRIVATE compiler_core coglet_backend_c)
if(COGLET_HAS_LLVM_BACKEND)
    target_link_libraries(coglet PRIVATE coglet_backend_llvm)
    target_compile_definitions(coglet PRIVATE COGLET_HAS_LLVM_BACKEND=1)
endif()

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
        NAME cog_ir_entry_lowering_dump
        COMMAND "${CMAKE_COMMAND}"
        -DEXE=$<TARGET_FILE:dump_ir>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/entry_lowering.cog
        -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/test_assets/ir/entry_lowering.expected
        -DEXPECT_EXIT=0
        -DOUTPUT_STREAM=stdout
        -P "${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake"
)
set_tests_properties(
        cog_ir_entry_lowering_dump
        PROPERTIES LABELS "ir;ir.lowering;ir.dump;metadata;entry"
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

# Host-C backend integration tests. These exercise frozen CogIR through the
# host-C emitter and native C toolchain. Registration helpers keep the backend
# mechanics explicit without duplicating the CMake invocation for every case.
include(cmake/HostCBackendTests.cmake)

# Explicit library/linker selection support. The library is intentionally
# outside the default link set so -L/-l forwarding tests do real work.
add_library(coglet_backend_link_support STATIC
        tests/support/backend_link_support.c
)

add_host_c_executable_test(
        backend_extern_c_default_symbol extern_c_default_symbol.cog 5
        "backend;backend.c;interop"
)
add_host_c_executable_test(
        backend_extern_c_symbol_override extern_c_symbol_override.cog 7
        "backend;backend.c;interop"
)
add_host_c_library_test(
        backend_checked_arithmetic checked_arithmetic.cog 95 joined
        "backend;backend.c;ir;checked-arithmetic"
)
add_host_c_executable_test(
        backend_structured_cfg structured_cfg.cog 101
        "backend;backend.c;ir;cfg;module-init"
)
add_host_c_executable_test(
        backend_control_flow_syntax control_flow_syntax.cog 55
        "backend;backend.c;ir;cfg;parser"
)
add_host_c_executable_test(
        backend_scalar_ops scalar_ops.cog 0
        "backend;backend.c;ir;scalar;bitwise;float;pointer"
)
add_host_c_library_test(
        backend_data_address_casts data_address_casts.cog 0 joined
        "backend;backend.c;ir;data;address;cast;volatile"
)
add_host_c_library_test(
        backend_indirect_calls indirect_calls.cog 0 joined
        "backend;backend.c;ir;abi;callback;indirect-call;variadic"
)
add_host_c_library_test(
        backend_aggregate_values aggregate_values.cog 0 joined
        "backend;backend.c;ir;aggregate;array;struct;global;interop"
)

foreach(cast_trap_case IN ITEMS integer float_integer float_narrow)
    add_host_c_trap_test(
            backend_checked_cast_${cast_trap_case}_trap
            checked_cast_${cast_trap_case}_trap.cog
            91
            "backend;backend.c;ir;cast;trap"
    )
endforeach()

add_host_c_executable_test(
        backend_shifts shifts.cog 0
        "backend;backend.c;ir;shift;checked-count"
)

foreach(shift_trap_case IN ITEMS negative_count wide_count)
    add_host_c_trap_test(
            backend_shift_${shift_trap_case}_trap
            shift_${shift_trap_case}_trap.cog
            91
            "backend;backend.c;ir;shift;checked-count;trap"
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

    add_host_c_trap_test(
            backend_checked_${trap_case}_trap
            checked_${trap_case}_trap.cog
            ${trap_exit}
            "backend;backend.c;ir;checked-arithmetic;trap"
    )
endforeach()

add_host_c_executable_test(
        backend_extern_c_pointer_types extern_c_pointer_types.cog 0
        "backend;backend.c;interop"
)
add_host_c_executable_test(
        backend_extern_c_string_literal extern_c_string_literal.cog 0
        "backend;backend.c;interop;strings"
        extern_c_string_literal.stdout
)
add_host_c_library_test(
        backend_explicit_library_split_flags extern_c_explicit_library.cog 23 split
        "backend;backend.c;interop;linker"
)
add_host_c_library_test(
        backend_explicit_library_joined_flags extern_c_explicit_library.cog 23 joined
        "backend;backend.c;interop;linker"
)
add_host_c_library_test(
        backend_extern_c_integer_aliases extern_c_integer_aliases.cog 29 joined
        "backend;backend.c;interop;abi"
)
add_host_c_library_test(
        backend_extern_c_scalar_aliases extern_c_scalar_aliases.cog 31 joined
        "backend;backend.c;interop;abi"
)
add_host_c_library_test(
        backend_wrapping_arithmetic wrapping_arithmetic.cog 93 joined
        "backend;backend.c;ir;wrapping"
)
add_host_c_library_test(
        backend_extern_c_repr_struct extern_c_repr_struct.cog 37 joined
        "backend;backend.c;interop;abi;struct"
)
add_host_c_library_test(
        backend_extern_c_repr_nested_struct extern_c_repr_nested_struct.cog 41 joined
        "backend;backend.c;interop;abi;struct"
)
add_host_c_library_test(
        backend_extern_c_repr_array_fields extern_c_repr_array_fields.cog 45 joined
        "backend;backend.c;interop;abi;struct;array"
)
add_host_c_library_test(
        backend_extern_c_repr_enum extern_c_repr_enum.cog 49 joined
        "backend;backend.c;interop;abi;enum;struct"
)
add_host_c_library_test(
        backend_extern_c_callback extern_c_callback.cog 53 joined
        "backend;backend.c;interop;abi;callback"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_host_c_library_test(
            backend_extern_c_calling_convention extern_c_calling_convention.cog 79 joined
            "backend;backend.c;interop;abi;calling-convention;callback"
    )
endif()

add_host_c_executable_test(
        backend_native_i32_entry native_i32_entry.cog 42
        "backend;backend.c;ir;entrypoint"
)
add_host_c_failure_test(
        backend_rejects_obsolete_c_int_entry obsolete_c_int_entry.cog 1
        "executable entry point must have signature 'main::() -> i32'"
        "backend;backend.c;backend.invalid;ir;entrypoint"
)
add_host_c_failure_test(
        backend_rejects_nested_main_as_entry nested_main_not_entry.cog 3
        "requires a top-level Coglet 'main' function"
        "backend;backend.c;backend.invalid;ir;entrypoint;nested-function"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND
   NOT WIN32)
    add_host_c_failure_test(
            backend_rejects_unsupported_stdcall unsupported_stdcall_calling_convention.cog 3
            "call=stdcall requires 32-bit x86"
            "backend;backend.c;backend.invalid;interop;abi;calling-convention"
    )
endif()

add_host_c_library_test(
        backend_extern_c_variadic extern_c_variadic.cog 57 joined
        "backend;backend.c;interop;abi;variadic"
)
add_host_c_library_test(
        backend_extern_c_incomplete_struct extern_c_incomplete_struct.cog 67 joined
        "backend;backend.c;interop;abi;struct;incomplete"
)
add_host_c_library_test(
        backend_extern_c_repr_union extern_c_repr_union.cog 71 joined
        "backend;backend.c;interop;abi;union;struct;array"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_host_c_library_test(
            backend_extern_c_repr_layout extern_c_repr_layout.cog 73 joined
            "backend;backend.c;interop;abi;layout;packed;alignment"
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

add_host_c_library_test(
        backend_extern_c_volatile_pointers extern_c_volatile_pointers.cog 89 joined
        "backend;backend.c;interop;abi;volatile"
)

# --- LLVM backend tests. ---
if(COGLET_HAS_LLVM_BACKEND)
    find_program(COGLET_CLANG_EXECUTABLE NAMES clang)

    add_test(
        NAME backend_llvm_stage1_scalar_cfg_entry
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage1_entry.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage1_entry.ll
        -DEXPECT_EXIT=11
        "-DEXPECT_IR_SUBSTRING=br i1"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage1_scalar_cfg_entry
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;cfg;entrypoint")

    add_test(
        NAME backend_llvm_stage1_module_init
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage1_module_init.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage1_module_init.ll
        -DEXPECT_EXIT=7
        "-DEXPECT_IR_SUBSTRING=call void @cog.fn.1()"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage1_module_init
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;module-init;entrypoint")

    add_test(
        NAME backend_llvm_stage2_checked_add
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage2_checked_add.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage2_checked_add.ll
        -DEXPECT_EXIT=3
        "-DEXPECT_IR_SUBSTRING=llvm.sadd.with.overflow.i32"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage2_checked_add
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;checked-arithmetic")

    add_test(
        NAME backend_llvm_stage2_integer_semantics
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage2_integer_semantics.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage2_integer_semantics.ll
        -DEXPECT_EXIT=0
        "-DEXPECT_IR_SUBSTRING=llvm.smul.with.overflow.i32"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage2_integer_semantics
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;integer;checked-arithmetic;wrapping;shift;cast")

    add_test(
        NAME backend_llvm_stage2_enum_integer
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage2_enum_integer.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage2_enum_integer.ll
        -DEXPECT_EXIT=0
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage2_enum_integer
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;integer;enum;cast")

    add_test(
        NAME backend_llvm_stage2_fibonacci
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage2_fibonacci.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage2_fibonacci.ll
        -DEXPECT_EXIT=55
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage2_fibonacci
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;integer;cfg;loop;entrypoint")

    foreach(trap_case IN ITEMS
        "checked_overflow_trap.cog|96|checked-overflow"
        "checked_division_by_zero_trap.cog|97|division-by-zero"
        "checked_division_overflow_trap.cog|98|division-overflow"
        "shift_negative_count_trap.cog|91|shift"
        "shift_wide_count_trap.cog|91|shift"
        "checked_cast_integer_trap.cog|91|cast"
        "llvm/unsigned_overflow_trap.cog|94|checked-overflow"
    )
        string(REPLACE "|" ";" trap_fields "${trap_case}")
        list(GET trap_fields 0 trap_input)
        list(GET trap_fields 1 trap_no_exit)
        list(GET trap_fields 2 trap_label)
        get_filename_component(trap_stem "${trap_input}" NAME_WE)
        add_test(
            NAME backend_llvm_stage2_${trap_stem}
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${trap_input}
            -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage2_${trap_stem}.ll
            -DNO_TRAP_EXIT=${trap_no_exit}
            -DCLANG=${COGLET_CLANG_EXECUTABLE}
            -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_trap_test.cmake"
        )
        set_tests_properties(backend_llvm_stage2_${trap_stem}
            PROPERTIES LABELS "backend;backend.llvm;ir;llvm;trap;${trap_label}")
    endforeach()

    add_test(
        NAME backend_llvm_stage3_pointer_compare
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage3_pointer_compare.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage3_pointer_compare.ll
        -DEXPECT_EXIT=0
        "-DEXPECT_IR_SUBSTRING=icmp eq ptr"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage3_pointer_compare
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;pointer;comparison")

    add_test(
        NAME backend_llvm_stage3_memory_aggregates
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage3_memory_aggregates.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage3_memory_aggregates.ll
        -DEXPECT_EXIT=73
        "-DEXPECT_IR_SUBSTRING=getelementptr inbounds"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage3_memory_aggregates
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;memory;pointer;array;struct;aggregate;module-init")

    add_test(
        NAME backend_llvm_stage3_extract_struct_value
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage3_extract_struct_value.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage3_extract_struct_value.ll
        -DEXPECT_EXIT=9
        "-DEXPECT_IR_SUBSTRING=extractvalue %cog.struct"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage3_extract_struct_value
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;struct;aggregate;extract")

    add_test(
        NAME backend_llvm_stage3_rejects_volatile_aggregate
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage3_rejects_volatile_aggregate.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage3_volatile_aggregate_unsupported.ll
        "-DEXPECT_SUBSTRING=volatile aggregate loads are outside the LLVM Stage 3 subset"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_failure_test.cmake"
    )
    set_tests_properties(backend_llvm_stage3_rejects_volatile_aggregate
        PROPERTIES LABELS "backend;backend.llvm;backend.invalid;ir;llvm;aggregate;volatile")

    add_test(
        NAME backend_llvm_stage3_rejects_repr_c_aggregate
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage3_rejects_repr_c_aggregate.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage3_repr_c_unsupported.ll
        "-DEXPECT_SUBSTRING=#repr(c) aggregate layout is outside the LLVM Stage 3 subset"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_failure_test.cmake"
    )
    set_tests_properties(backend_llvm_stage3_rejects_repr_c_aggregate
        PROPERTIES LABELS "backend;backend.llvm;backend.invalid;ir;llvm;aggregate;interop;abi")

    add_test(
        NAME backend_llvm_stage4_float_semantics
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage4_float_semantics.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage4_float_semantics.ll
        -DEXPECT_EXIT=0
        "-DEXPECT_IR_SUBSTRING=fcmp une"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage4_float_semantics
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;float;cast;comparison")

    add_test(
        NAME backend_llvm_stage4_switch
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage4_switch.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage4_switch.ll
        -DEXPECT_EXIT=29
        "-DEXPECT_IR_SUBSTRING=switch i32"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage4_switch
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;cfg;switch;enum;bool")

    add_test(
        NAME backend_llvm_stage4_indirect_native_call
        COMMAND "${CMAKE_COMMAND}"
        -DCOMPILER=$<TARGET_FILE:coglet>
        -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/llvm/stage4_indirect_native_call.cog
        -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage4_indirect_native_call.ll
        -DEXPECT_EXIT=42
        "-DEXPECT_IR_SUBSTRING=call i32 %"
        -DCLANG=${COGLET_CLANG_EXECUTABLE}
        -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_executable_test.cmake"
    )
    set_tests_properties(backend_llvm_stage4_indirect_native_call
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;function-value;indirect-call")

    foreach(float_trap_case IN ITEMS
        "checked_cast_float_integer_trap.cog|91|float-to-int"
        "checked_cast_float_narrow_trap.cog|91|float-narrow"
        "llvm/nan_to_integer_trap.cog|91|float-to-int"
    )
        string(REPLACE "|" ";" float_trap_fields "${float_trap_case}")
        list(GET float_trap_fields 0 float_trap_input)
        list(GET float_trap_fields 1 float_trap_no_exit)
        list(GET float_trap_fields 2 float_trap_label)
        get_filename_component(float_trap_stem "${float_trap_input}" NAME_WE)
        add_test(
            NAME backend_llvm_stage4_${float_trap_stem}
            COMMAND "${CMAKE_COMMAND}"
            -DCOMPILER=$<TARGET_FILE:coglet>
            -DINPUT=${CMAKE_SOURCE_DIR}/tests/test_assets/backend/${float_trap_input}
            -DOUTPUT_IR=${CMAKE_BINARY_DIR}/llvm_stage4_${float_trap_stem}.ll
            -DNO_TRAP_EXIT=${float_trap_no_exit}
            -DCLANG=${COGLET_CLANG_EXECUTABLE}
            -P "${CMAKE_SOURCE_DIR}/cmake/run_llvm_trap_test.cmake"
        )
        set_tests_properties(backend_llvm_stage4_${float_trap_stem}
            PROPERTIES LABELS "backend;backend.llvm;ir;llvm;float;cast;trap;${float_trap_label}")
    endforeach()

    add_test(
        NAME backend_llvm_stage4_explicit_trap_terminator
        COMMAND $<TARGET_FILE:check_llvm_stage4_trap>
                ${CMAKE_BINARY_DIR}/llvm_stage4_explicit_trap.ll
    )
    set_tests_properties(backend_llvm_stage4_explicit_trap_terminator
        PROPERTIES LABELS "backend;backend.llvm;ir;llvm;cfg;trap")
endif()
