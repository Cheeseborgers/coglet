foreach(required_var IN ITEMS COMPILER INPUTS EXPECT_EXIT EXPECT_SUBSTRING)
    if(NOT DEFINED ${required_var})
        message(FATAL_ERROR "run_multifile_failure_test.cmake requires ${required_var}")
    endif()
endforeach()

string(REPLACE "|" ";" input_list "${INPUTS}")
execute_process(
    COMMAND "${COMPILER}" ${input_list}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL EXPECT_EXIT)
    message(FATAL_ERROR
        "multi-file failure test exited ${result}, expected ${EXPECT_EXIT}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}"
    )
endif()

string(FIND "${stderr}" "${EXPECT_SUBSTRING}" found)
if(found EQUAL -1)
    message(FATAL_ERROR
        "stderr did not contain '${EXPECT_SUBSTRING}'\n"
        "stderr:\n${stderr}"
    )
endif()

if(DEFINED EXPECT_PATH_SUBSTRING AND NOT "${EXPECT_PATH_SUBSTRING}" STREQUAL "")
    string(FIND "${stderr}" "${EXPECT_PATH_SUBSTRING}" path_found)
    if(path_found EQUAL -1)
        message(FATAL_ERROR
            "stderr did not contain source path '${EXPECT_PATH_SUBSTRING}'\n"
            "stderr:\n${stderr}"
        )
    endif()
endif()
