if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_IR OR NOT DEFINED NO_TRAP_EXIT)
    message(FATAL_ERROR "run_llvm_trap_test.cmake requires COMPILER, INPUT, OUTPUT_IR, and NO_TRAP_EXIT")
endif()
execute_process(
    COMMAND "${COMPILER}" "${INPUT}" --emit-llvm "${OUTPUT_IR}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR "LLVM emission failed (${emit_result}):\n${emit_stdout}${emit_stderr}")
endif()

file(READ "${OUTPUT_IR}" llvm_ir)
string(FIND "${llvm_ir}" "llvm.trap" trap_pos)
if(trap_pos EQUAL -1)
    message(FATAL_ERROR "LLVM trap test emitted no llvm.trap path:\n${llvm_ir}")
endif()

if(NOT DEFINED CLANG OR CLANG STREQUAL "" OR CLANG MATCHES "-NOTFOUND$")
    return()
endif()

set(output_exe "${OUTPUT_IR}.exe")
execute_process(
    COMMAND "${CLANG}" -Wno-override-module "${OUTPUT_IR}" -o "${output_exe}"
    RESULT_VARIABLE clang_result
    OUTPUT_VARIABLE clang_stdout
    ERROR_VARIABLE clang_stderr
)
if(NOT clang_result EQUAL 0)
    message(FATAL_ERROR "clang failed to compile emitted LLVM IR (${clang_result}):\n${clang_stdout}${clang_stderr}")
endif()

execute_process(
    COMMAND "${output_exe}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
if("${run_result}" STREQUAL "0" OR "${run_result}" STREQUAL "${NO_TRAP_EXIT}")
    message(FATAL_ERROR
        "LLVM executable did not trap; result was ${run_result}\n"
        "stdout:\n${run_stdout}\n"
        "stderr:\n${run_stderr}"
    )
endif()
