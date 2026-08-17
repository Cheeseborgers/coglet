foreach(required_var IN ITEMS COMPILER MAIN IO_MODULE STDLIB_ROOT OUTPUT EXPECT_SUBSTRING)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "run_missing_runtime_test.cmake requires ${required_var}")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${COMPILER}" "${MAIN}" "${IO_MODULE}"
            --stdlib-root "${STDLIB_ROOT}"
            -o "${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 3)
    message(FATAL_ERROR
        "runtime-missing compile exited ${result}, expected 3\n"
        "stdout:\n${stdout}\nstderr:\n${stderr}"
    )
endif()

string(FIND "${stderr}" "${EXPECT_SUBSTRING}" found)
if(found EQUAL -1)
    message(FATAL_ERROR
        "runtime-missing diagnostic did not contain '${EXPECT_SUBSTRING}'\n"
        "stderr:\n${stderr}"
    )
endif()

if(EXISTS "${OUTPUT}")
    message(FATAL_ERROR "runtime-missing compile unexpectedly created '${OUTPUT}'")
endif()
