# ── Standalone Catch2 test suite for crosspad-core ──
# Compiles portable core sources only — no FreeRTOS, ESP-IDF, or SDL2.
# Uses SyncEventBus (std::mutex) instead of FreeRtosEventBus.

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

# ── Portable core sources (exclude platform-specific files) ──
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(CORE_SOURCES
    # App system
    ${CORE_DIR}/src/app/AppRegistry.cpp

    # Event bus (synchronous — no FreeRTOS/ESP-IDF)
    ${CORE_DIR}/src/event/SyncEventBus.cpp

    # Music theory
    ${CORE_DIR}/src/music/MusicTheory.cpp

    # Pad system
    ${CORE_DIR}/src/pad/PadManager.cpp
    ${CORE_DIR}/src/pad/PadLedController.cpp
    ${CORE_DIR}/src/pad/PadAnimator.cpp
    ${CORE_DIR}/src/pad/RGBCallbackManager.cpp

    # Platform (portable parts only)
    ${CORE_DIR}/src/platform/CrosspadPlatformInit.cpp
    ${CORE_DIR}/src/platform/PlatformCapabilities.cpp
    ${CORE_DIR}/src/platform/PlatformServices.cpp

    # Settings
    ${CORE_DIR}/src/settings/CrosspadSettings.cpp
    ${CORE_DIR}/src/settings/ISettingsUI.cpp

    # Protocol
    ${CORE_DIR}/src/protocol/Stm32CommandSender.cpp
    ${CORE_DIR}/src/protocol/Stm32MessageHandler.cpp

    # MIDI routing
    ${CORE_DIR}/src/midi/MidiInputHandler.cpp
)

# ── Test sources ──
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(crosspad_core_tests ${TEST_SOURCES} ${CORE_SOURCES})

target_compile_definitions(crosspad_core_tests PRIVATE
    PLATFORM_PC=1
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
)

target_include_directories(crosspad_core_tests PRIVATE
    ${CORE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}  # test_helpers.hpp
)

target_link_libraries(crosspad_core_tests PRIVATE
    Catch2::Catch2
    ArduinoJson
)

include(CTest)
include(Catch)
catch_discover_tests(crosspad_core_tests)
