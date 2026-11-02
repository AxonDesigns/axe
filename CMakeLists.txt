cmake_minimum_required(VERSION 3.28)
project(axe VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Axe builds with Clang only (got ${CMAKE_C_COMPILER_ID} / "
                        "${CMAKE_CXX_COMPILER_ID}). Use the CMake presets or set "
                        "CMAKE_C_COMPILER=clang and CMAKE_CXX_COMPILER=clang++. C++ is only "
                        "used to build HarfBuzz.")
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UI_HOOKS "Compile tracepoints in (a relaxed atomic load each when unused)" ON)
option(UI_BUILD_TESTS "Build the test suite" ON)
option(UI_BUILD_EXAMPLES "Build the examples" ON)
option(UI_FUZZ "Build the libFuzzer targets (use the fuzz preset)" OFF)
set(UI_SANITIZE "" CACHE STRING "Semicolon-separated sanitizers, e.g. address;undefined")

include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(tools)

if(UI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(UI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(UI_FUZZ)
        add_subdirectory(fuzz)
    endif()
endif()
add_subdirectory(bench)
