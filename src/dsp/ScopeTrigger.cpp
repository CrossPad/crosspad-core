// SPDX-License-Identifier: MIT
#include "crosspad/dsp/ScopeTrigger.hpp"

#include <cmath>

namespace crosspad {

namespace {
constexpr float kMsPerSecond = 1000.0f;
}

uint32_t scopeWindowSamples(float hz, uint32_t sampleRate) {
    if (sampleRate == 0) return 0;

    float ms = kScopeMaxWindowMs;
    if (hz > 0.0f) ms = static_cast<float>(kScopePeriods) * kMsPerSecond / hz;
    if (ms < kScopeMinWindowMs) ms = kScopeMinWindowMs;
    if (ms > kScopeMaxWindowMs) ms = kScopeMaxWindowMs;

    const float samples = ms * static_cast<float>(sampleRate) / kMsPerSecond;
    return static_cast<uint32_t>(std::lround(samples));
}

ScopeWindow scopeTrigger(const float* buf, uint32_t n, uint32_t length) {
    ScopeWindow w;
    if (!buf || n == 0 || length == 0) return w;

    // A window the snapshot cannot fill is shortened rather than honoured: the
    // caller draws [start, start + length) and must not be handed a range that
    // runs past the buffer it owns.
    if (length > n) {
        w.length = n;
        return w;
    }
    w.length = length;
    w.start = n - length;

    // Backwards from the latest start that still leaves a whole window, so the
    // newest edge wins: an older one would show a wave that is already history.
    for (uint32_t i = n - length; i >= 1; --i) {
        if (buf[i - 1] < -kScopeHysteresis && buf[i] >= kScopeHysteresis) {
            w.start = i;
            w.triggered = true;
            break;
        }
    }
    return w;
}

} // namespace crosspad
