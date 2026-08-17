foreach(required_var IN ITEMS COMPILER INPUTS OUTPUT EXPECT_EXIT BACKEND)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_multifile_executable_test.cmake requires ${required_var}")
    endif()
endforeach()

string(REPLACE "|" ";" input_list "${INPUTS}")
file(REMOVE "${OUTPUT}")

set(command "${COMPILER}" ${input_list} -o "${OUTPUT}")
if(BACKEND STREQUAL "llvm")
    list(APPEND command --backend llvm)
elseif(NOT BACKEND STREQUAL "host-c")
    message(FATAL_ERROR "BACKEND must be host-c or llvm")
endif()
if(DEFINED DEBUG_INFO AND DEBUG_INFO)
    list(APPEND command -g)
endif()
if(DEFINED OPT_LEVEL AND NOT "${OPT_LEVEL}" STREQUAL "")
    list(APPEND command "-O${OPT_LEVEL}")
endif()

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "multi-file ${BACKEND} compilation failed with exit ${compile_result}\n"
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
if(NOT run_result MATCHES "^[0-9]+$" OR NOT run_result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "multi-file executable exited ${run_result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
