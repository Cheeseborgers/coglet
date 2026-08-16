if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_IR)
    message(FATAL_ERROR "COMPILER, INPUT, and OUTPUT_IR are required")
endif()

if(NOT DEFINED OPT_LEVEL OR NOT OPT_LEVEL MATCHES "^[1-3]$")
    message(FATAL_ERROR "OPT_LEVEL must be 1, 2, or 3")
endif()

if(NOT DEFINED EXPECT_SUBSTRING OR NOT DEFINED ABSENT_SUBSTRING)
    message(FATAL_ERROR "EXPECT_SUBSTRING and ABSENT_SUBSTRING are required")
endif()

file(REMOVE "${OUTPUT_IR}")
execute_process(
    COMMAND "${COMPILER}" "${INPUT}" --emit-llvm "${OUTPUT_IR}" "-O${OPT_LEVEL}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR
        "optimized LLVM emission failed with exit ${emit_result}\n"
        "stdout:\n${emit_stdout}\n"
        "stderr:\n${emit_stderr}"
    )
endif()

file(READ "${OUTPUT_IR}" llvm_ir)
foreach(required IN ITEMS "target datalayout =" "target triple =" "define" "@main()")
    string(FIND "${llvm_ir}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "optimized LLVM IR is missing '${required}':\n${llvm_ir}")
    endif()
endforeach()

string(FIND "${llvm_ir}" "${EXPECT_SUBSTRING}" expected_found)
if(expected_found EQUAL -1)
    message(FATAL_ERROR "optimized LLVM IR is missing '${EXPECT_SUBSTRING}':\n${llvm_ir}")
endif()

string(FIND "${llvm_ir}" "${ABSENT_SUBSTRING}" absent_found)
if(NOT absent_found EQUAL -1)
    message(FATAL_ERROR "optimized LLVM IR unexpectedly contains '${ABSENT_SUBSTRING}':\n${llvm_ir}")
endif()
