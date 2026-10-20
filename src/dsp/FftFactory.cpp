// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/FftFactory.hpp"

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
FftBackend g_backend = nullptr;

} // namespace

void setFftBackend(FftBackend backend) {
    std::lock_guard<std::mutex> lock(factoryMutex());
    g_backend = backend;
}

IFft& createFft(const FftMemory& mem) {
    std::lock_guard<std::mutex> lock(factoryMutex());

    // Asked on every call: a backend that could not serve once failed for
    // want of memory, and that condition passes. See FftFactory.hpp.
    if (g_backend) {
        if (IFft* fft = g_backend(mem)) {
            g_current = fft;
            return *fft;
        }
    }

    g_current = &portable();
    return portable();
}

IFft* currentFft() {
    std::lock_guard<std::mutex> lock(factoryMutex());
    return g_current;
}

} // namespace crosspad
