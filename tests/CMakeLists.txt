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
    ${CORE_DIR}/src/settings/SettingsIo.cpp
    ${CORE_DIR}/src/settings/SettingsScope.cpp

    # Network
    ${CORE_DIR}/src/net/WifiNetworkList.cpp

    # Protocol
    ${CORE_DIR}/src/protocol/Stm32CommandSender.cpp
    ${CORE_DIR}/src/protocol/Stm32MessageHandler.cpp

    # MIDI routing
    ${CORE_DIR}/src/midi/MidiInputHandler.cpp
    ${CORE_DIR}/src/midi/MidiClock.cpp

    # STM32 config sync
    ${CORE_DIR}/src/stm32/Stm32ConfigSync.cpp

    # Kit JSON and sample-path resolution
    ${CORE_DIR}/src/kit/KitJsonParser.cpp
    ${CORE_DIR}/src/kit/KitPathUtils.cpp

    # DSP (FFT seam + factory, pitch detection, rate conversion).
    # EspDspFft.cpp is here on purpose: without esp-dsp it compiles to nothing,
    # and building it on the host is what keeps that guard honest.
    ${CORE_DIR}/src/dsp/PortableFft.cpp
    ${CORE_DIR}/src/dsp/EspDspFft.cpp
    ${CORE_DIR}/src/dsp/FftFactory.cpp
    ${CORE_DIR}/src/dsp/PitchDetector.cpp
    ${CORE_DIR}/src/dsp/FftResampler.cpp
    ${CORE_DIR}/src/dsp/ScopeTrigger.cpp

    # Pitched instrument engine
    ${CORE_DIR}/src/instrument/WavPcm.cpp
    ${CORE_DIR}/src/instrument/SampleBank.cpp
    ${CORE_DIR}/src/instrument/PitchedInstrument.cpp
    ${CORE_DIR}/src/instrument/PitchedKitLoader.cpp

    # Audio file writing
    ${CORE_DIR}/src/audio/WavWriter.cpp

    # Audio song model
    ${CORE_DIR}/src/song/AudioSong.cpp
    ${CORE_DIR}/src/song/LiveSet.cpp
    ${CORE_DIR}/src/song/SongEngine.cpp

    # Sample tools
    ${CORE_DIR}/src/sampletools/SliceMath.cpp
    ${CORE_DIR}/src/sampletools/SampleFx.cpp

    # Firmware updates
    ${CORE_DIR}/src/fw/Semver.cpp
    ${CORE_DIR}/src/fw/FwKit.cpp
    ${CORE_DIR}/src/fw/FwSchedule.cpp
    ${CORE_DIR}/src/fw/FwCatalog.cpp
    ${CORE_DIR}/src/fw/FwManifest.cpp
    ${CORE_DIR}/src/fw/FwLibraryGc.cpp

    # Loop recorder
    ${CORE_DIR}/src/sequencer/PatternSequencer.cpp
    ${CORE_DIR}/src/sequencer/SequenceStore.cpp
)

# ── Test sources ──
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(crosspad_core_tests ${TEST_SOURCES} ${CORE_SOURCES})

target_compile_definitions(crosspad_core_tests PRIVATE
    PLATFORM_PC=1
    CROSSPAD_CORE_DIR="${CORE_DIR}"
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
)

target_include_directories(crosspad_core_tests PRIVATE
    ${CORE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}  # test_helpers.hpp
)

# The Live Set tests read the skeleton the board inflates from ROM
find_package(ZLIB REQUIRED)

target_link_libraries(crosspad_core_tests PRIVATE
    Catch2::Catch2
    ArduinoJson
    ZLIB::ZLIB
)

include(CTest)
include(Catch)
catch_discover_tests(crosspad_core_tests)
