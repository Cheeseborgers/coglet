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
        src/diag.c
        src/semantic_anal.c
)
target_include_directories(compiler_core PUBLIC include)

# --- The real compiler binary. ---
add_executable(coglet src/coglet.c ${COMMON_SOURCES})
target_link_libraries(coglet PRIVATE compiler_core)

# --- The test runner binary. ---
add_executable(coglet_test_runner src/coglet_test_runner.c ${COMMON_SOURCES})
target_link_libraries(coglet PRIVATE compiler_core)

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
    file(GLOB test_sources ${dir}/*.cog)
    foreach(test_src ${test_sources})
        get_filename_component(test_name ${test_src} NAME_WE)
        set(expected_file ${dir}/${test_name}.expected)

        if(EXISTS ${expected_file})
            add_test(
                    NAME ${prefix}_${test_name}
                    COMMAND ${CMAKE_COMMAND}
                    -DEXE=$<TARGET_FILE:${exe_target}>
                    -DINPUT=${test_src}
                    -DEXPECTED=${expected_file}
                    -P ${CMAKE_SOURCE_DIR}/cmake/run_diff_test.cmake
            )
        else()
            message(WARNING "No .expected file for ${test_src}, skipping test")
        endif()
    endforeach()
endfunction()

add_diff_tests(lexer  ${CMAKE_SOURCE_DIR}/tests/lexer  dump_tokens)
add_diff_tests(parser ${CMAKE_SOURCE_DIR}/tests/parser dump_ast)

# Copy assets
add_custom_command(
        TARGET coglet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/tests/test_assets
        $<TARGET_FILE_DIR:coglet>/test_assets
)