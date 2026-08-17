foreach(required_var IN ITEMS COMPILER INPUT OUTPUT_ASM)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_llvm_assembly_test.cmake requires ${required_var}")
    endif()
endforeach()

file(REMOVE "${OUTPUT_ASM}")
if(DEFINED EXECUTABLE_OUTPUT AND NOT "${EXECUTABLE_OUTPUT}" STREQUAL "")
    file(REMOVE "${EXECUTABLE_OUTPUT}")
endif()
if(DEFINED ASSEMBLED_OUTPUT AND NOT "${ASSEMBLED_OUTPUT}" STREQUAL "")
    file(REMOVE "${ASSEMBLED_OUTPUT}")
endif()

set(compiler_args "${INPUT}" --emit-asm "${OUTPUT_ASM}")
if(DEFINED OPT_LEVEL AND NOT "${OPT_LEVEL}" STREQUAL "")
    list(APPEND compiler_args "-O${OPT_LEVEL}")
endif()
if(DEFINED DEBUG_INFO AND DEBUG_INFO)
    list(APPEND compiler_args -g)
endif()
if(DEFINED EXECUTABLE_OUTPUT AND NOT "${EXECUTABLE_OUTPUT}" STREQUAL "")
    list(APPEND compiler_args -o "${EXECUTABLE_OUTPUT}" --backend llvm)
endif()

execute_process(
    COMMAND "${COMPILER}" ${compiler_args}
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Coglet assembly emission failed with exit ${compile_result}\n"
        "stdout:\n${compile_stdout}\n"
        "stderr:\n${compile_stderr}"
    )
endif()

if(NOT EXISTS "${OUTPUT_ASM}")
    message(FATAL_ERROR "Coglet did not create assembly output '${OUTPUT_ASM}'")
endif()
file(SIZE "${OUTPUT_ASM}" assembly_size)
if(assembly_size EQUAL 0)
    message(FATAL_ERROR "assembly output '${OUTPUT_ASM}' is empty")
endif()

if(DEFINED EXPECT_ASM_SUBSTRING AND NOT "${EXPECT_ASM_SUBSTRING}" STREQUAL "")
    file(READ "${OUTPUT_ASM}" assembly_text)
    string(FIND "${assembly_text}" "${EXPECT_ASM_SUBSTRING}" substring_index)
    if(substring_index EQUAL -1)
        message(FATAL_ERROR
            "assembly output '${OUTPUT_ASM}' does not contain '${EXPECT_ASM_SUBSTRING}'"
        )
    endif()
endif()

if(DEFINED ASSEMBLED_OUTPUT AND NOT "${ASSEMBLED_OUTPUT}" STREQUAL "")
    if(NOT DEFINED CC OR "${CC}" STREQUAL "")
        message(FATAL_ERROR "ASSEMBLED_OUTPUT requires CC")
    endif()
    if(NOT DEFINED EXPECT_EXIT)
        message(FATAL_ERROR "ASSEMBLED_OUTPUT requires EXPECT_EXIT")
    endif()

    execute_process(
        COMMAND "${CC}" "${OUTPUT_ASM}" -o "${ASSEMBLED_OUTPUT}"
        RESULT_VARIABLE assemble_result
        OUTPUT_VARIABLE assemble_stdout
        ERROR_VARIABLE assemble_stderr
    )
    if(NOT assemble_result EQUAL 0)
        message(FATAL_ERROR
            "host compiler failed to assemble/link '${OUTPUT_ASM}' with exit ${assemble_result}\n"
            "stdout:\n${assemble_stdout}\n"
            "stderr:\n${assemble_stderr}"
        )
    endif()

    execute_process(
        COMMAND "${ASSEMBLED_OUTPUT}"
        RESULT_VARIABLE assembled_run_result
        OUTPUT_VARIABLE assembled_stdout
        ERROR_VARIABLE assembled_stderr
    )
    if(NOT assembled_run_result EQUAL EXPECT_EXIT)
        message(FATAL_ERROR
            "assembled executable exited ${assembled_run_result}; expected ${EXPECT_EXIT}\n"
            "stdout:\n${assembled_stdout}\n"
            "stderr:\n${assembled_stderr}"
        )
    endif()
endif()

if(DEFINED EXECUTABLE_OUTPUT AND NOT "${EXECUTABLE_OUTPUT}" STREQUAL "")
    if(NOT DEFINED EXPECT_EXIT)
        message(FATAL_ERROR "EXECUTABLE_OUTPUT requires EXPECT_EXIT")
    endif()
    if(NOT EXISTS "${EXECUTABLE_OUTPUT}")
        message(FATAL_ERROR "Coglet did not create executable output '${EXECUTABLE_OUTPUT}'")
    endif()

    execute_process(
        COMMAND "${EXECUTABLE_OUTPUT}"
        RESULT_VARIABLE executable_result
        OUTPUT_VARIABLE executable_stdout
        ERROR_VARIABLE executable_stderr
    )
    if(NOT executable_result EQUAL EXPECT_EXIT)
        message(FATAL_ERROR
            "Coglet executable exited ${executable_result}; expected ${EXPECT_EXIT}\n"
            "stdout:\n${executable_stdout}\n"
            "stderr:\n${executable_stderr}"
        )
    endif()
endif()
