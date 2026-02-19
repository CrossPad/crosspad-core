# CrossPad Core — ESP-IDF Component CMakeLists.txt
#
# When used as an ESP-IDF component (via git submodule in components/crosspad-core/),
# this file registers the portable core library.
#
# Platform-specific implementations (EspEventBus, etc.) live in a separate
# component (e.g., components/crosspad-platform-idf/) that depends on this one.

# Collect portable core sources
file(GLOB_RECURSE CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${CORE_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    REQUIRES
        bblanchon__ArduinoJson
        freertos
        esp_event
)
