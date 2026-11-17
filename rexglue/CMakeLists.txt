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
    src/http_client.cpp
    src/online.cpp
    src/trials_menu.cpp
    src/keyboard_input.cpp
    src/game_exit.cpp
    src/post_effects.cpp
    src/dlc_loader.cpp
)

if(WIN32)
    add_executable(trialshd WIN32 ${TRIALSHD_SOURCES})
else()
    add_executable(trialshd ${TRIALSHD_SOURCES})
endif()

rexglue_setup_target(trialshd GPU_PLUGINS xenos)

# Build for Title Update 4 instead of the original release: codegen must run on a default.xex
# patched with tools/apply_title_update.py and trialshd_config_tu4.toml (the CI workflow's
# "game_version: tu4" option does both). Hook addresses come from src/game_version.h.
option(TRIALSHD_TU4 "Build for Trials HD Title Update 4" OFF)
if(TRIALSHD_TU4)
    target_compile_definitions(trialshd PRIVATE TRIALSHD_TU4=1)
endif()
