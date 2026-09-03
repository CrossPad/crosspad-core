// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file FftFactory.hpp
 * @brief The one FFT a process uses, chosen once at first call.
 *
 * A platform does not pick an implementation: it hands over memory and asks
 * for an IFft. The factory prefers EspDspFft when this build has esp-dsp
 * (ESP-IDF and Arduino-ESP32 both do) and its shared twiddle table can be
 * built; otherwise PortableFft, which needs nothing.
 *
 * **A failed esp-dsp table does not latch.** The only runtime reason it fails
 * is the table's memory not being there, and that is scarcest exactly when
 * the first caller arrives -- during a kit load, with zones already resident.
 * Every createFft() call retries, so a later load gets the fast kernel back
 * instead of being told for the life of the process that this board has no
 * esp-dsp.
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
 * @brief The process-wide FFT, created on first call.
 *
 * Safe to call repeatedly and from more than one thread. The returned
 * reference stays valid for the life of the process; a later call may return
 * a *different* implementation if an esp-dsp table that previously failed
 * has since been built, so callers that cache the reference keep a working
 * FFT either way.
 */
IFft& createFft(const FftMemory& mem);

/// The FFT createFft() settled on, or nullptr if it has never been called.
/// For code that needs the one that already exists and has no memory to offer.
IFft* currentFft();

} // namespace crosspad
