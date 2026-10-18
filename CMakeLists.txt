# CrossPad Core — portable C++ library.
#
# One source list, two build systems:
#   - ESP-IDF component, as a git submodule in components/crosspad-core/;
#   - plain CMake, top-level or through add_subdirectory(): target
#     crosspad-core (static library). Link ArduinoJson as target ArduinoJson
#     before adding this directory to use your own copy; otherwise it is
#     fetched.
#
# Platform-specific implementations (EspEventBus, EspDspFft, Drv2605Driver,
# FreeRtosEventBus, ...) live in the platform: components/crosspad-platform-idf/
# on ESP-IDF, src/pc_stubs/ in crosspad-pc. An accelerated FFT or render kernel
# is registered at runtime (setFftBackend(), dsp::setAudioKernels()), so no
# vendor header or ISA code is reachable from core.

file(GLOB_RECURSE CROSSPAD_CORE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${CROSSPAD_CORE_SOURCES}
        INCLUDE_DIRS
            "${CMAKE_CURRENT_LIST_DIR}/include"
        REQUIRES
            bblanchon__ArduinoJson
    )
    return()
endif()

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(crosspad-core LANGUAGES CXX)
endif()

# AppEntry carries the LVGL create/destroy callbacks only under USE_LVGL, so
# core and every consumer must agree on it: PUBLIC, from this one option.
option(CROSSPAD_CORE_USE_LVGL "Build crosspad-core and its consumers with USE_LVGL" OFF)

if(NOT TARGET ArduinoJson)
    include(FetchContent)
    FetchContent_Declare(
        ArduinoJson
        GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
        GIT_TAG        v7.3.0
    )
    FetchContent_MakeAvailable(ArduinoJson)
endif()

add_library(crosspad-core STATIC ${CROSSPAD_CORE_SOURCES})
target_include_directories(crosspad-core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(crosspad-core PUBLIC cxx_std_17)
target_link_libraries(crosspad-core PUBLIC ArduinoJson)
if(CROSSPAD_CORE_USE_LVGL)
    target_compile_definitions(crosspad-core PUBLIC USE_LVGL)
endif()
