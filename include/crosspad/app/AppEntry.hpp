#pragma once

/**
 * @file AppEntry.hpp
 * @brief Portable application entry descriptor
 *
 * Pure data struct describing an application's metadata and function pointers.
 * No platform dependencies — only uses forward declarations for LVGL and App types.
 */

#include <cstdint>

#ifdef USE_LVGL
// Forward declaration to avoid full LVGL header dependency.
// Consumers that create/use these function pointers include lvgl.h themselves.
// Guard: skip if lvgl.h was already included (it defines LV_VERSION_CHECK).
#ifndef LV_VERSION_CHECK
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
#endif
#endif

// Forward declaration — App class is defined by the platform (LVGL or Simple GUI variant)
class App;

namespace crosspad {

/**
 * @brief Describes a registered application's metadata and callbacks
 *
 * Used by AppRegistry to store application information at compile time.
 * Function pointers are set by the REGISTER_APP macro or manually at runtime.
 */
struct AppEntry {
    const char* name;
    const char* icon;

#ifdef USE_LVGL
    /// LVGL UI factory — creates the app's widget tree
    lv_obj_t* (*createLVGL)(lv_obj_t* parent, App* a) = nullptr;

    /// LVGL UI teardown — destroys the app's widget tree
    void (*destroyLVGL)(lv_obj_t* app) = nullptr;
#endif

    /// Launcher order (higher = earlier; equal priorities keep registration order).
    int priority = 0;

    /// Pad-logic name auto-activated on app start
    /// and restored on destroy (see AppBase hooks).
    /// nullptr = app manages pad logic manually.
    const char* padLogic = nullptr;

    /// Launcher tints the icon one flat colour. For monochrome glyphs
    /// (black on transparent); coloured icons stay raw.
    bool recolorIcon = false;
};

} // namespace crosspad
