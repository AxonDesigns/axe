# The C and TypeScript versions of an example must draw byte-identical headless images at the
# same times under both schedulers (tests/CMakeLists.txt: layout_cards_equivalence).
#
#   cmake -DC=<c binary> -DTS=<ts binary> -DOUT=<dir> -DTIMES="0;1.3" [-DARGS="--dark;--rtl"] -P equivalence.cmake
file(MAKE_DIRECTORY "${OUT}")
set(failures 0)
foreach(scheduler single threaded)
    foreach(at IN LISTS TIMES)
        set(c_out "${OUT}/c-${scheduler}-${at}.ppm")
        set(ts_out "${OUT}/ts-${scheduler}-${at}.ppm")
        execute_process(COMMAND ${CMAKE_COMMAND} -E env UI_SCHEDULER=${scheduler}
                                "${C}" --headless --at ${at} --out "${c_out}" ${ARGS}
                        RESULT_VARIABLE c_status OUTPUT_QUIET)
        execute_process(COMMAND "${TS}" --headless --at ${at} --out "${ts_out}" --strict
                                --scheduler ${scheduler} -- ${ARGS}
                        RESULT_VARIABLE ts_status OUTPUT_QUIET)
        if(NOT c_status EQUAL 0 OR NOT ts_status EQUAL 0)
            message(SEND_ERROR "${scheduler} at ${at}s: C exited ${c_status}, TypeScript ${ts_status}")
            math(EXPR failures "${failures} + 1")
            continue()
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${c_out}" "${ts_out}" RESULT_VARIABLE differ)
        if(differ)
            message(SEND_ERROR "${scheduler} at ${at}s: the images differ (${c_out}, ${ts_out})")
            math(EXPR failures "${failures} + 1")
        else()
            message(STATUS "${scheduler} at ${at}s: identical")
        endif()
    endforeach()
endforeach()
if(failures)
    message(FATAL_ERROR "${failures} comparison(s) failed")
endif()
