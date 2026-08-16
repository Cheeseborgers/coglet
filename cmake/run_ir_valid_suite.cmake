if(NOT DEFINED EXE OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "EXE and FIXTURE_DIR are required")
endif()

file(GLOB_RECURSE fixtures LIST_DIRECTORIES false "${FIXTURE_DIR}/*.cog")
list(SORT fixtures)

set(fixture_count 0)
foreach(fixture IN LISTS fixtures)
    execute_process(
        COMMAND "${EXE}" "${fixture}"
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_VARIABLE stderr_text
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "dump_ir failed for ${fixture} with exit ${result}:\n${stderr_text}")
    endif()
    math(EXPR fixture_count "${fixture_count} + 1")
endforeach()

if(fixture_count EQUAL 0)
    message(FATAL_ERROR "No semantic-valid fixtures found under ${FIXTURE_DIR}")
endif()

message(STATUS "dump_ir accepted ${fixture_count} semantic-valid fixtures")
