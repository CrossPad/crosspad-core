#pragma once

/**
 * @file AppPointers.hpp
 * @brief Smart pointer infrastructure for memory-safe App management
 *
 * Phase 2: Safety & Quality - Memory Management
 *
 * This header provides type-safe smart pointer wrappers for App instances,
 * eliminating manual new/delete and preventing memory leaks.
 *
 * Key improvements:
 * - std::unique_ptr for exclusive ownership (replaces raw App*)
 * - std::shared_ptr for async operations (LVGL callbacks)
 * - std::weak_ptr for non-owning observers
 * - Exception-safe factory functions
 *
 * @author CrossPad SOTA Modernization - Phase 2
 * @date 2026-02-16
 */

#include <memory>
#include <array>
#include <vector>

// Forward declaration
class App;

namespace crosspad {

/**
 * @brief Smart pointer type aliases for clarity and consistency
 */

/// Unique ownership of App instance (single owner)
using AppPtr = std::unique_ptr<App>;

/// Shared ownership of App instance (multiple owners, refcounted)
/// Use for async operations where App lifetime extends beyond scope
using SharedAppPtr = std::shared_ptr<App>;

/// Non-owning reference to App (detects if App was destroyed)
using WeakAppPtr = std::weak_ptr<App>;

/**
 * @brief Container types for App storage
 */

/// Fixed-size array of Apps with unique ownership
/// Use for AppManager::lvglApps storage (compile-time size known)
template<size_t N>
using AppArray = std::array<AppPtr, N>;

/// Dynamic vector of Apps with shared ownership
/// Use when size is unknown at compile-time or apps need shared access
using AppVector = std::vector<SharedAppPtr>;

/**
 * @brief Exception-safe factory function for App creation
 *
 * Replaces: App* app = new App(...);
 * With:     auto app = makeApp(...);
 *
 * Benefits:
 * - No manual delete required (RAII)
 * - Exception-safe (no leak if constructor throws)
 * - Clear ownership semantics
 *
 * @tparam Args Constructor argument types (deduced)
 * @param args Arguments forwarded to App constructor
 * @return std::unique_ptr<App> owning the created App
 *
 * @example
 * ```cpp
 * auto app = makeApp(parent, "MyApp", "/icon.png", createFn, destroyFn);
 * // app is std::unique_ptr<App>, auto-deleted when out of scope
 * ```
 */
template<typename... Args>
inline AppPtr makeApp(Args&&... args) {
    return std::make_unique<App>(std::forward<Args>(args)...);
}

/**
 * @brief Create shared App instance for async operations
 *
 * Use when App needs to survive beyond immediate scope, e.g.:
 * - LVGL async callbacks (lv_async_call)
 * - Event handlers that capture App reference
 * - Multi-threaded access to App
 *
 * @tparam Args Constructor argument types (deduced)
 * @param args Arguments forwarded to App constructor
 * @return std::shared_ptr<App> with reference counting
 *
 * @example
 * ```cpp
 * auto app = makeSharedApp(parent, "MyApp", ...);
 * lv_async_call([app](void*) {
 *     app->doSomething();  // Safe: app kept alive by shared_ptr
 * }, nullptr);
 * ```
 */
template<typename... Args>
inline SharedAppPtr makeSharedApp(Args&&... args) {
    return std::make_shared<App>(std::forward<Args>(args)...);
}

/**
 * @brief Helper to convert unique_ptr to shared_ptr when needed
 *
 * Use sparingly - prefer creating SharedAppPtr directly if async needed.
 *
 * @param unique_app Unique pointer to transfer ownership from
 * @return std::shared_ptr<App> with transferred ownership
 *
 * @note This consumes the unique_ptr (moves ownership)
 */
inline SharedAppPtr toShared(AppPtr&& unique_app) {
    return SharedAppPtr(std::move(unique_app));
}

/**
 * @brief RAII guard for App lifecycle management
 *
 * Ensures App is properly started and destroyed even if exceptions occur.
 *
 * @example
 * ```cpp
 * {
 *     AppLifecycleGuard guard(app.get());
 *     app->start();  // Guard ensures destroyApp() called on scope exit
 *     // ... use app ...
 * }  // app->destroyApp() called automatically
 * ```
 */
class AppLifecycleGuard {
public:
    explicit AppLifecycleGuard(App* app) : app_(app), started_(false) {}

    ~AppLifecycleGuard() {
        if (app_ && started_) {
            // Safe to call destroyApp even if already destroyed (idempotent)
            app_->destroyApp();
        }
    }

    // Mark app as started (enables auto-destroy on scope exit)
    void markStarted() { started_ = true; }

    // Cancel auto-destroy (app will be managed elsewhere)
    void release() { app_ = nullptr; }

    // Delete copy operations (RAII guard should not be copied)
    AppLifecycleGuard(const AppLifecycleGuard&) = delete;
    AppLifecycleGuard& operator=(const AppLifecycleGuard&) = delete;

private:
    App* app_;
    bool started_;
};

} // namespace crosspad

/**
 * @brief Migration guide for existing code
 *
 * OLD (manual memory management):
 * ```cpp
 * App* app = new App(parent, "Name", "/icon.png", createFn, destroyFn);
 * // ... use app ...
 * delete app;  // Easy to forget!
 * ```
 *
 * NEW (smart pointers):
 * ```cpp
 * auto app = crosspad::makeApp(parent, "Name", "/icon.png", createFn, destroyFn);
 * // ... use app ...
 * // No delete needed - RAII auto-cleanup
 * ```
 *
 * OLD (storing raw pointers):
 * ```cpp
 * App* lvglApps[16];
 * lvglApps[0] = new App(...);
 * ```
 *
 * NEW (storing unique_ptr):
 * ```cpp
 * crosspad::AppArray<16> lvglApps;
 * lvglApps[0] = crosspad::makeApp(...);
 * ```
 *
 * OLD (async callback - unsafe):
 * ```cpp
 * App* app = ...;
 * lv_async_call([](void* data) {
 *     App* a = static_cast<App*>(data);
 *     a->destroyApp();  // CRASH if app deleted before callback!
 * }, app);
 * ```
 *
 * NEW (async callback - safe):
 * ```cpp
 * auto app = crosspad::makeSharedApp(...);
 * lv_async_call([app_copy = app](void*) mutable {
 *     app_copy->destroyApp();  // Safe: shared_ptr keeps alive
 * }, nullptr);
 * ```
 */

