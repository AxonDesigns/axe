# A hot update's scene equals a fresh build of the new version driven to the same state: the
# two runs of tests/hot/hot_scene.test.ts draw byte-identical images, under both schedulers.
file(MAKE_DIRECTORY "${OUT}")
foreach(scheduler single threaded)
    foreach(mode hot fresh)
        execute_process(COMMAND "${RUNNER}" --dev "${OUT}/${scheduler}-${mode}" --scheduler ${scheduler}
                                --arg ${mode} --arg "${OUT}/${scheduler}-${mode}.ppm" "${DRIVER}"
                        RESULT_VARIABLE status)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "${scheduler} ${mode}: exited ${status}")
        endif()
    endforeach()
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/${scheduler}-hot.ppm" "${OUT}/${scheduler}-fresh.ppm"
                    RESULT_VARIABLE differ)
    if(differ)
        message(FATAL_ERROR "${scheduler}: the updated scene differs from a fresh build's")
    endif()
endforeach()

# The overlay is drawn over the last scene and goes away without a trace.
execute_process(COMMAND "${RUNNER}" --dev "${OUT}/overlay" --arg overlay --arg "${OUT}/overlay" "${DRIVER}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "overlay: exited ${status}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/overlay-before.ppm" "${OUT}/overlay-after.ppm" RESULT_VARIABLE differ)
if(differ)
    message(FATAL_ERROR "fixing the error didn't give the same scene back")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/overlay-before.ppm" "${OUT}/overlay-error.ppm" RESULT_VARIABLE differ OUTPUT_QUIET ERROR_QUIET)
if(NOT differ)
    message(FATAL_ERROR "the overlay drew nothing")
endif()
