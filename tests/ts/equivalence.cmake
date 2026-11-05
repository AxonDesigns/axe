# The C, TypeScript, TSX and Luau versions of an example must draw byte-identical headless images
# at the same times under both schedulers (tests/CMakeLists.txt: layout_cards_equivalence,
# spring_rect_equivalence).
#
#   cmake -DC=<c binary> -DTS=<ts binary> [-DTSX=<tsx binary>] [-DLUAU=<luau binary>] -DOUT=<dir>
#         -DTIMES="0;1.3"
#         [-DARGS="--dark;--rtl"] -P equivalence.cmake
file(MAKE_DIRECTORY "${OUT}")
set(failures 0)
set(ports TS)
foreach(port TSX LUAU)
    if(${port})
        list(APPEND ports ${port})
    endif()
endforeach()
foreach(scheduler single threaded)
    foreach(at IN LISTS TIMES)
        set(c_out "${OUT}/c-${scheduler}-${at}.ppm")
        execute_process(COMMAND ${CMAKE_COMMAND} -E env UI_SCHEDULER=${scheduler}
                                "${C}" --headless --at ${at} --out "${c_out}" ${ARGS}
                        RESULT_VARIABLE c_status OUTPUT_QUIET)
        if(NOT c_status EQUAL 0)
            message(SEND_ERROR "${scheduler} at ${at}s: C exited ${c_status}")
            math(EXPR failures "${failures} + 1")
            continue()
        endif()
        foreach(port IN LISTS ports)
            string(TOLOWER "${port}" name)
            set(port_out "${OUT}/${name}-${scheduler}-${at}.ppm")
            execute_process(COMMAND "${${port}}" --headless --at ${at} --out "${port_out}" --strict
                                    --scheduler ${scheduler} -- ${ARGS}
                            RESULT_VARIABLE status OUTPUT_QUIET)
            if(NOT status EQUAL 0)
                message(SEND_ERROR "${scheduler} at ${at}s: ${port} exited ${status}")
                math(EXPR failures "${failures} + 1")
                continue()
            endif()
            execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${c_out}" "${port_out}" RESULT_VARIABLE differ)
            if(differ)
                message(SEND_ERROR "${scheduler} at ${at}s: ${port}'s image differs from C's (${c_out}, ${port_out})")
                math(EXPR failures "${failures} + 1")
            else()
                message(STATUS "${scheduler} at ${at}s: ${port} identical")
            endif()
        endforeach()
    endforeach()
endforeach()
if(failures)
    message(FATAL_ERROR "${failures} comparison(s) failed")
endif()
