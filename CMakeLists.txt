# CrossPad Core — ESP-IDF Component CMakeLists.txt
#
# When used as an ESP-IDF component (via git submodule in components/crosspad-core/),
# this file registers the portable core library.
#
# Platform-specific implementations (EspEventBus, EspDspFft, Drv2605Driver,
# etc.) live in a separate component (components/crosspad-platform-idf/) that
# depends on this one.

# Collect portable core sources
file(GLOB_RECURSE CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# esp-dsp is not a platform dependency: it ships with ESP-IDF *and* with
# Arduino-ESP32, so the source that uses it (the SIMD pan accumulate in
# PitchedInstrument) lives here and is guarded by
# CROSSPAD_USE_ESP_DSP && __has_include(<esp_dsp.h>). Without both the
# portable path takes over.
if(IDF_TARGET STREQUAL "esp32s3")
    set(CORE_DSP_REQUIRES espressif__esp-dsp)
else()
    set(CORE_DSP_REQUIRES "")
endif()

idf_component_register(
    SRCS ${CORE_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    REQUIRES
        bblanchon__ArduinoJson
        freertos
        esp_event
        ${CORE_DSP_REQUIRES}
)

if(IDF_TARGET STREQUAL "esp32s3")
    # PUBLIC, not PRIVATE: dsp/AudioKernels.hpp picks its loops from these
    # macros, inline, so core and every consumer that includes it have to
    # agree on them or one inline function has two definitions.
    # CROSSPAD_SIMD_PIE: the S3's 128-bit float loads and stores (EE.LDF.128 /
    # EE.STF.128), which an ESP32 or an S2 does not have.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CROSSPAD_USE_ESP_DSP=1 CROSSPAD_SIMD_PIE=1)
endif()
