foreach(required_var IN ITEMS COMPILER INPUT OUTPUT_IR EXPECT_SOURCE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_module_discovery_llvm_ir_test.cmake requires ${required_var}")
    endif()
endforeach()

file(REMOVE "${OUTPUT_IR}")
set(command "${COMPILER}" "${INPUT}")
if(DEFINED SEARCH_DIR AND NOT "${SEARCH_DIR}" STREQUAL "")
    list(APPEND command -I "${SEARCH_DIR}")
endif()
if(DEFINED STDLIB_ROOT AND NOT "${STDLIB_ROOT}" STREQUAL "")
    list(APPEND command --stdlib-root "${STDLIB_ROOT}")
endif()
list(APPEND command --emit-llvm "${OUTPUT_IR}" -g)
execute_process(
    COMMAND ${command}
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
