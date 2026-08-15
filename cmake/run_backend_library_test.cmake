if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED LIB_DIR)
    message(FATAL_ERROR "LIB_DIR is required")
endif()

if(NOT DEFINED LIB_NAME)
    message(FATAL_ERROR "LIB_NAME is required")
endif()

if(NOT DEFINED STYLE)
    message(FATAL_ERROR "STYLE is required")
endif()

if(NOT DEFINED EXPECT_EXIT)
    set(EXPECT_EXIT 23)
endif()

file(REMOVE "${OUTPUT}")

if(STYLE STREQUAL "split")
    execute_process(
        COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}" -L "${LIB_DIR}" -l "${LIB_NAME}"
        RESULT_VARIABLE compile_result
        OUTPUT_VARIABLE compile_stdout
        ERROR_VARIABLE compile_stderr
    )
elseif(STYLE STREQUAL "joined")
    execute_process(
        COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}" "-L${LIB_DIR}" "-l${LIB_NAME}"
        RESULT_VARIABLE compile_result
        OUTPUT_VARIABLE compile_stdout
        ERROR_VARIABLE compile_stderr
    )
else()
    message(FATAL_ERROR "unknown STYLE '${STYLE}'")
endif()

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Coglet explicit-library compilation failed with exit ${compile_result}\n"
        "stdout:\n${compile_stdout}\n"
        "stderr:\n${compile_stderr}"
    )
endif()

execute_process(
    COMMAND "${OUTPUT}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)

if(NOT run_result MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "generated executable did not exit normally: ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()

if(NOT run_result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "generated executable exited ${run_result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
