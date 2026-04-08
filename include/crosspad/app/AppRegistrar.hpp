#pragma once

/**
 * @file AppRegistrar.hpp
 * @brief Registration helpers and REGISTER_APP macros
 *
 * Two registration strategies are provided:
 *
 * 1. REGISTER_APP (default) — creates a named function `_register_<name>_app()`
 *    that the platform calls at init time. Reliable on all platforms.
 *    ESP-IDF: generate_registry.cmake scans sources and generates AppRegistry_InitAll().
 *    Arduino/Linux: can scan or call explicitly.
 *
 * 2. AppRegistrar class — static-constructor pattern. Use directly on platforms
 *    where static initializers are guaranteed to be preserved (Arduino, Linux).
 *    NOT reliable on ESP-IDF due to component linker stripping.
 */

#include "crosspad/app/AppRegistry.hpp"

namespace crosspad {

/**
 * @brief Helper class whose constructor registers an app at static init time
 *
 * Use on platforms where static constructors are reliable (Arduino, Linux).
 * On ESP-IDF, use REGISTER_APP macro + generate_registry.cmake instead.
 */
class AppRegistrar {
public:
    AppRegistrar(const AppEntry& entry) {
        AppRegistry::getInstance().registerApp(entry);
    }
};

} // namespace crosspad

// ============================================================================
// Registration macros
// ============================================================================

/**
 * @brief Full registration macro — static constructor pattern
 *
 * Creates a static AppRegistrar whose constructor registers the app
 * at program startup. Works on all platforms when combined with
 * --whole-archive linker flag for library archives (see appLibrary
 * flag in library.json and force_link_app_libs.py).
 *
 * Parameters:
 *   appName      — Identifier (used as stringified name and variable suffix)
 *   logicFunc    — void (*)(App*) app logic callback, or nullptr
 *   iconPath     — Icon path string (e.g. "/assets/play.png")
 *   createLVGL   — LVGL factory function, or nullptr
 *   destroyLVGL  — LVGL destroy function, or nullptr
 *   initSimple   — Simple GUI init function, or nullptr
 *   refreshFunc  — GUI refresh function, or nullptr
 *   cleanupFunc  — Cleanup function, or nullptr
 *   priority     — int, higher = more preferred for auto-selection
 */
#ifndef REGISTER_APP
#ifdef CROSSPAD_REGISTER_APP_NAMED_FUNCTIONS
// Named-function pattern: creates _register_<appName>_app() callable by platform init.
// Required on ESP-IDF where static constructors are unreliable.
#ifdef USE_LVGL
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority) \
    static crosspad::AppRegistrar reg##appName(crosspad::AppEntry{ \
        #appName, iconPath, createLVGL, destroyLVGL, logicFunc, initSimple, refreshFunc, cleanupFunc, priority \
    });
#else
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority) \
    static crosspad::AppRegistrar reg##appName(crosspad::AppEntry{ \
        #appName, "", logicFunc, initSimple, refreshFunc, cleanupFunc, priority \
    });
#endif
#else
// Static constructor pattern: works on platforms with reliable static init (Arduino, Linux).
#ifdef USE_LVGL
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, iconPath, createLVGL, destroyLVGL, logicFunc, initSimple, refreshFunc, cleanupFunc, priority \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#else
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, "", logicFunc, initSimple, refreshFunc, cleanupFunc, priority \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#endif
#endif // CROSSPAD_REGISTER_APP_NAMED_FUNCTIONS
#endif // REGISTER_APP

// --- Convenience macros ---

/** Register LVGL app with icon and factory functions */
#ifdef USE_LVGL
#define REGISTER_LVGL_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL) \
    REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, nullptr, nullptr, nullptr, 0)
#else
#define REGISTER_LVGL_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL) \
    /* LVGL disabled — app not registered */
#endif

/** Register Simple GUI app with initialization function */
#define REGISTER_SIMPLE_APP(appName, logicFunc, initSimple) \
    REGISTER_APP(appName, logicFunc, "", nullptr, nullptr, initSimple, nullptr, nullptr, 0)

/** Register Simple GUI app with initialization and cleanup */
#define REGISTER_SIMPLE_APP_WITH_CLEANUP(appName, logicFunc, initSimple, cleanupFunc) \
    REGISTER_APP(appName, logicFunc, "", nullptr, nullptr, initSimple, nullptr, cleanupFunc, 0)

/** Register Simple GUI app with refresh function */
#define REGISTER_SIMPLE_APP_WITH_REFRESH(appName, logicFunc, initSimple, refreshFunc) \
    REGISTER_APP(appName, logicFunc, "", nullptr, nullptr, initSimple, refreshFunc, nullptr, 0)

/** Register Simple GUI app with priority */
#define REGISTER_SIMPLE_APP_WITH_PRIORITY(appName, logicFunc, initSimple, priority) \
    REGISTER_APP(appName, logicFunc, "", nullptr, nullptr, initSimple, nullptr, nullptr, priority)

/** Register Simple GUI app with refresh function and priority */
#define REGISTER_SIMPLE_APP_WITH_REFRESH_AND_PRIORITY(appName, logicFunc, initSimple, refreshFunc, priority) \
    REGISTER_APP(appName, logicFunc, "", nullptr, nullptr, initSimple, refreshFunc, nullptr, priority)
