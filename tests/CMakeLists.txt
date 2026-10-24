# ── Standalone Catch2 test suite for crosspad-core ──
# Links crosspad-core as a plain CMake library — no FreeRTOS, ESP-IDF, or SDL2.
# Uses SyncEventBus (std::mutex) as the event bus.

cmake_minimum_required(VERSION 3.14)
project(crosspad-core-tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Catch2 ──
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(Catch2)

# ── ArduinoJson (required by CrosspadSettings) ──
FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG        v7.3.0
)
FetchContent_MakeAvailable(ArduinoJson)

# ── crosspad-core itself: the same source list the ESP-IDF component builds ──
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_subdirectory(${CORE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/crosspad-core)

# ── Test sources ──
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(crosspad_core_tests ${TEST_SOURCES})

target_compile_definitions(crosspad_core_tests PRIVATE
    PLATFORM_PC=1
    CROSSPAD_CORE_DIR="${CORE_DIR}"
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
)

target_include_directories(crosspad_core_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}  # test_helpers.hpp
)

# The Live Set tests read the skeleton the board inflates from ROM
find_package(ZLIB REQUIRED)

target_link_libraries(crosspad_core_tests PRIVATE
    crosspad-core
    Catch2::Catch2
    ZLIB::ZLIB
)

include(CTest)
include(Catch)
catch_discover_tests(crosspad_core_tests)
