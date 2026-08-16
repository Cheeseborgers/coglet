if(NOT DEFINED COMPILER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_IR OR NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "run_llvm_executable_test.cmake requires COMPILER, INPUT, OUTPUT_IR, and EXPECT_EXIT")
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
foreach(required IN ITEMS "target datalayout =" "target triple =" "define i32 @main()" "cog.fn.")
    string(FIND "${llvm_ir}" "${required}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "LLVM IR is missing required text '${required}':\n${llvm_ir}")
    endif()
endforeach()

if(DEFINED EXPECT_IR_SUBSTRING)
    string(FIND "${llvm_ir}" "${EXPECT_IR_SUBSTRING}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "LLVM IR is missing '${EXPECT_IR_SUBSTRING}':\n${llvm_ir}")
    endif()
endif()

if(DEFINED CLANG AND NOT CLANG STREQUAL "" AND NOT CLANG MATCHES "-NOTFOUND$")
    set(output_exe "${OUTPUT_IR}.exe")
    set(clang_command "${CLANG}" -Wno-override-module "${OUTPUT_IR}")
    if(DEFINED LINK_INPUT AND NOT LINK_INPUT STREQUAL "")
        list(APPEND clang_command "${LINK_INPUT}")
    endif()
    list(APPEND clang_command -o "${output_exe}")
    execute_process(
        COMMAND ${clang_command}
        RESULT_VARIABLE clang_result
        OUTPUT_VARIABLE clang_stdout
        ERROR_VARIABLE clang_stderr
    )
    if(NOT clang_result EQUAL 0)
        message(FATAL_ERROR "clang failed to compile emitted LLVM IR (${clang_result}):\n${clang_stdout}${clang_stderr}")
    endif()
    execute_process(COMMAND "${output_exe}" RESULT_VARIABLE run_result)
    if(NOT run_result EQUAL EXPECT_EXIT)
        message(FATAL_ERROR "LLVM executable exited ${run_result}, expected ${EXPECT_EXIT}")
    endif()
endif()
