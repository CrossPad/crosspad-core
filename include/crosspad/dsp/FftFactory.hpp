// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file FftFactory.hpp
 * @brief The one FFT a process uses, chosen once at first call.
 *
 * A caller does not pick an implementation: it hands over memory and asks
 * for an IFft. The factory prefers the backend the platform registered with
 * setFftBackend() (the ESP-IDF platform registers esp-dsp's) whenever that
 * backend can serve; otherwise PortableFft, which needs nothing.
 *
 * **A backend that cannot serve does not latch.** esp-dsp's only runtime
 * reason is its twiddle table's memory not being there, and that is scarcest
 * exactly when the first caller arrives -- during a kit load, with zones
 * already resident. Every createFft() call asks the backend again, so a later
 * load gets the fast kernel back instead of being told for the life of the
 * process that this board has none.
 */

#include "crosspad/dsp/IFft.hpp"

#include <cstddef>

namespace crosspad {

/**
 * @brief Where an FFT backend's tables come from.
 *
 * @c alloc takes an alignment because esp-dsp's assembly kernels move complex
 * pairs with 64-bit loads; the table is asked for at 16 bytes. A platform that
 * has no aligned allocator may ignore the argument as long as it returns at
 * least natural alignment -- the transform then runs, only slower.
 * @c log is optional; it receives one already-formatted line.
 */
struct FftMemory {
    void* (*alloc)(size_t bytes, size_t align) = nullptr;
    void  (*free)(void* p) = nullptr;
    void  (*log)(const char* line) = nullptr;
};

/**
 * @brief A platform's accelerated FFT.
 *
 * Called by createFft() with that caller's memory. Returns the backend's one
 * instance, or nullptr while it cannot serve; it is asked again on the next
 * createFft() call.
 */
using FftBackend = IFft* (*)(const FftMemory& mem);

/// Register the platform's FFT backend before the first createFft() call;
/// nullptr leaves only PortableFft.
void setFftBackend(FftBackend backend);

/**
 * @brief The process-wide FFT, created on first call.
 *
 * Safe to call repeatedly and from more than one thread. The returned
 * reference stays valid for the life of the process; a later call may return
 * a *different* implementation if a backend that could not serve before has
 * since become able to, so callers that cache the reference keep a working
 * FFT either way.
 */
IFft& createFft(const FftMemory& mem);

/// The FFT createFft() settled on, or nullptr if it has never been called.
/// For code that needs the one that already exists and has no memory to offer.
IFft* currentFft();

} // namespace crosspad
