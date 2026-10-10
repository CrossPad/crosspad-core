// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/FftFactory.hpp"

#include "crosspad/dsp/EspDspFft.hpp"
#include "crosspad/dsp/PortableFft.hpp"

#include <mutex>

namespace crosspad {

namespace {

std::mutex& factoryMutex() {
    static std::mutex m;
    return m;
}

// Function-local statics rather than file-scope objects: this is a library
// that a platform links whether or not it ever asks for an FFT, and a
// PortableFft at file scope would build nothing but still run a constructor
// before main().
PortableFft& portable() {
    static PortableFft fft;
    return fft;
}

IFft* g_current = nullptr;

} // namespace

IFft& createFft(const FftMemory& mem) {
    std::lock_guard<std::mutex> lock(factoryMutex());

#if CROSSPAD_HAS_ESP_DSP_FFT
    // Built with the memory of the *first* caller; a later caller's allocator
    // is ignored, since the table esp-dsp keeps is a single global anyway.
    static EspDspFft esp(mem);
    // Retried on every call: a table that failed once failed for want of
    // memory, and that condition passes. See FftFactory.hpp.
    if (esp.available()) {
        g_current = &esp;
        return esp;
    }
#endif

    (void)mem;
    g_current = &portable();
    return portable();
}

IFft* currentFft() {
    std::lock_guard<std::mutex> lock(factoryMutex());
    return g_current;
}

} // namespace crosspad
