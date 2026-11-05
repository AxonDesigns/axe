# A bundle holds no trace of hot reload: the transform's registrations aren't there, and @axe/ui's
# dev hooks are dead code esbuild removed.
file(READ "${BUNDLE}" text)
string(LENGTH "${text}" size)
if(size LESS 1000)
    message(FATAL_ERROR "${BUNDLE} looks empty")
endif()
foreach(symbol AXE_DEV _$hot ownerlessSink instanceBegin hotSetHandler hotBegin nearestHot swapOwner errorSink keepSignal redefineTemplate dropUnusedVariants)
    string(FIND "${text}" "${symbol}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "${BUNDLE} holds ${symbol}")
    endif()
endforeach()
