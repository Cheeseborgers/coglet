foreach(required_var IN ITEMS COMPILER INPUTS OUTPUT_IR EXPECT_FIRST_SOURCE EXPECT_SECOND_SOURCE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_multifile_llvm_ir_test.cmake requires ${required_var}")
    endif()
endforeach()

string(REPLACE "|" ";" input_list "${INPUTS}")
file(REMOVE "${OUTPUT_IR}")

execute_process(
    COMMAND "${COMPILER}" ${input_list} --emit-llvm "${OUTPUT_IR}" -g
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "multi-file LLVM IR emission failed with exit ${result}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}"
    )
endif()

file(READ "${OUTPUT_IR}" ir)
foreach(expected IN ITEMS "${EXPECT_FIRST_SOURCE}" "${EXPECT_SECOND_SOURCE}")
    string(FIND "${ir}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "LLVM debug IR did not contain source '${expected}'")
    endif()
endforeach()
