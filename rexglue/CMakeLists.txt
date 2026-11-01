# trialshd - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(trialshd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TRIALSHD_SOURCES
    src/main.cpp
    src/crash_handler.cpp
    src/perf_tools.cpp
)

if(WIN32)
    add_executable(trialshd WIN32 ${TRIALSHD_SOURCES})
else()
    add_executable(trialshd ${TRIALSHD_SOURCES})
endif()

rexglue_setup_target(trialshd GPU_PLUGINS xenos)
