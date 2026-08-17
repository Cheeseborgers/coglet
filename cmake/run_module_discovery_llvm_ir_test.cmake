foreach(required_var IN ITEMS COMPILER INPUT SEARCH_DIR OUTPUT_IR EXPECT_SOURCE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_module_discovery_llvm_ir_test.cmake requires ${required_var}")
    endif()
endforeach()

file(REMOVE "${OUTPUT_IR}")
execute_process(
    COMMAND "${COMPILER}" "${INPUT}" -I "${SEARCH_DIR}" --emit-llvm "${OUTPUT_IR}" -g
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "discovered-source LLVM IR emission failed: ${result}\n${stderr}")
endif()
file(READ "${OUTPUT_IR}" ir)
string(FIND "${ir}" "${EXPECT_SOURCE}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "LLVM debug IR did not contain discovered source '${EXPECT_SOURCE}'")
endif()
