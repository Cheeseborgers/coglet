if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_IR)
    message(FATAL_ERROR "COMPILER, INPUT, and OUTPUT_IR are required")
endif()

set(command "${COMPILER}" "${INPUT}" --emit-llvm "${OUTPUT_IR}" -g)
if(DEFINED OPT_LEVEL)
    if(NOT OPT_LEVEL MATCHES "^[0-3]$")
        message(FATAL_ERROR "OPT_LEVEL must be 0, 1, 2, or 3")
    endif()
    list(APPEND command "-O${OPT_LEVEL}")
endif()

file(REMOVE "${OUTPUT_IR}")
execute_process(
    COMMAND ${command}
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR
        "LLVM debug-info emission failed with exit ${emit_result}\n"
        "stdout:\n${emit_stdout}\n"
        "stderr:\n${emit_stderr}"
    )
endif()

file(READ "${OUTPUT_IR}" llvm_ir)
foreach(index RANGE 1 16)
    set(required_name "REQUIRED_TEXT_${index}")
    if(DEFINED ${required_name})
        string(FIND "${llvm_ir}" "${${required_name}}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR
                "LLVM debug IR is missing '${${required_name}}':\n${llvm_ir}"
            )
        endif()
    endif()

    set(absent_name "ABSENT_TEXT_${index}")
    if(DEFINED ${absent_name})
        string(FIND "${llvm_ir}" "${${absent_name}}" found)
        if(NOT found EQUAL -1)
            message(FATAL_ERROR
                "LLVM debug IR unexpectedly contains '${${absent_name}}':\n${llvm_ir}"
            )
        endif()
    endif()
endforeach()
