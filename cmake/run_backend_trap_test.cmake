if(NOT DEFINED COMPILER)
    message(FATAL_ERROR "COMPILER is required")
endif()

if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED NO_TRAP_EXIT)
    message(FATAL_ERROR "NO_TRAP_EXIT is required")
endif()

file(REMOVE "${OUTPUT}")

execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Coglet backend compilation failed with exit ${compile_result}\n"
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

if("${run_result}" STREQUAL "0" OR
   "${run_result}" STREQUAL "${NO_TRAP_EXIT}")
    message(FATAL_ERROR
        "generated executable did not trap; result was ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
