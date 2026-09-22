#pragma once

/**
 * @file AppRegistrar.hpp
 * @brief Registration helpers and REGISTER_APP macros
 *
 * REGISTER_APP creates a named function `_register_<name>_app()` that the
 * platform registry generator calls during initialization.
 */

#include "crosspad/app/AppRegistry.hpp"

// ============================================================================
// Registration macros
// ============================================================================

/**
 * @brief Full registration macro
 *
 * Creates a named registration function invoked by the platform registry.
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
 *   ...          — optional bool recolorIcon (see AppEntry::recolorIcon), default false
 */
#ifndef REGISTER_APP
#ifdef USE_LVGL
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority, ...) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, iconPath, createLVGL, destroyLVGL, priority, nullptr, ##__VA_ARGS__ \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#else
#define REGISTER_APP(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority, ...) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, "", priority, nullptr, ##__VA_ARGS__ \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#endif
#endif // REGISTER_APP

/**
 * @brief Full registration macro with pad-logic auto-activation
 *
 * Same as REGISTER_APP, plus a 10th argument naming the IPadLogicHandler
 * to activate on app start and restore on destroy (see AppEntry::padLogic).
 *
 * Parameters: same as REGISTER_APP, plus:
 *   padLogicName — const char*, name passed to PadManager::registerPadLogic(),
 *                  or nullptr if the app manages pad logic manually.
 *   ...          — optional bool recolorIcon, as for REGISTER_APP
 */
#ifndef REGISTER_APP_PL
#ifdef USE_LVGL
#define REGISTER_APP_PL(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority, padLogicName, ...) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, iconPath, createLVGL, destroyLVGL, priority, padLogicName, ##__VA_ARGS__ \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#else
#define REGISTER_APP_PL(appName, logicFunc, iconPath, createLVGL, destroyLVGL, initSimple, refreshFunc, cleanupFunc, priority, padLogicName, ...) \
    void _register_##appName##_app() { \
        static const crosspad::AppEntry entry = { \
            #appName, "", priority, padLogicName, ##__VA_ARGS__ \
        }; \
        crosspad::AppRegistry::getInstance().registerApp(entry); \
    }
#endif
#endif // REGISTER_APP_PL
