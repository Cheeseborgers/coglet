if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "COMPILER, INPUT, and OUTPUT are required")
endif()

if(NOT DEFINED OPT_LEVEL OR NOT OPT_LEVEL MATCHES "^[1-3]$")
    message(FATAL_ERROR "OPT_LEVEL must be 1, 2, or 3")
endif()

if(NOT DEFINED NO_TRAP_EXIT)
    message(FATAL_ERROR "NO_TRAP_EXIT is required")
endif()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -o "${OUTPUT}" --backend llvm "-O${OPT_LEVEL}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "optimized LLVM native trap compilation failed with exit ${compile_result}\n"
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
if("${run_result}" STREQUAL "0" OR "${run_result}" STREQUAL "${NO_TRAP_EXIT}")
    message(FATAL_ERROR
        "optimized LLVM native executable did not trap; result was ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
