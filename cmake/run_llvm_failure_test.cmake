if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_IR OR NOT DEFINED EXPECT_SUBSTRING)
    message(FATAL_ERROR "run_llvm_failure_test.cmake missing required arguments")
endif()
execute_process(
    COMMAND "${COMPILER}" "${INPUT}" --emit-llvm "${OUTPUT_IR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(result EQUAL 0)
    message(FATAL_ERROR "LLVM emission unexpectedly succeeded")
endif()
string(FIND "${stderr}" "${EXPECT_SUBSTRING}" pos)
if(pos EQUAL -1)
    message(FATAL_ERROR "expected stderr substring '${EXPECT_SUBSTRING}', got:\n${stderr}")
endif()
