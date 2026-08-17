if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "EXPECT_EXIT is required")
endif()

file(REMOVE "${OUTPUT}")

set(compile_command "${COMPILER}" "${INPUT}" -o "${OUTPUT}" --backend llvm)
if(DEFINED OPT_LEVEL)
    if(NOT OPT_LEVEL MATCHES "^[0-3]$")
        message(FATAL_ERROR "OPT_LEVEL must be 0, 1, 2, or 3")
    endif()
    list(APPEND compile_command "-O${OPT_LEVEL}")
endif()
if(DEFINED DEBUG_INFO AND DEBUG_INFO)
    list(APPEND compile_command -g)
endif()
if(DEFINED LIB_DIR OR DEFINED LIB_NAME OR DEFINED STYLE)
    if(NOT DEFINED LIB_DIR OR NOT DEFINED LIB_NAME OR NOT DEFINED STYLE)
        message(FATAL_ERROR "LIB_DIR, LIB_NAME, and STYLE must be provided together")
    endif()

    if(STYLE STREQUAL "split")
        list(APPEND compile_command -L "${LIB_DIR}" -l "${LIB_NAME}")
    elseif(STYLE STREQUAL "joined")
        list(APPEND compile_command "-L${LIB_DIR}" "-l${LIB_NAME}")
    else()
        message(FATAL_ERROR "unknown STYLE '${STYLE}'")
    endif()
endif()

execute_process(
    COMMAND ${compile_command}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Coglet LLVM native compilation failed with exit ${compile_result}\n"
        "stdout:\n${compile_stdout}\n"
        "stderr:\n${compile_stderr}"
    )
endif()

if(DEFINED EXPECT_DEBUG_STRING_REGEX)
    file(STRINGS "${OUTPUT}" debug_strings REGEX "${EXPECT_DEBUG_STRING_REGEX}" LIMIT_COUNT 1)
    if(debug_strings STREQUAL "")
        message(FATAL_ERROR
            "LLVM native executable contains no debug string matching '${EXPECT_DEBUG_STRING_REGEX}'"
        )
    endif()
endif()

execute_process(
    COMMAND "${OUTPUT}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)

if(NOT run_result MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "generated LLVM native executable did not exit normally: ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()

if(NOT run_result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "generated LLVM native executable exited ${run_result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
