// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file WaveStatus.hpp
 * @brief The waveform verb (board WAVE_STATUS, simulator wave_status),
 *        gathered once.
 *
 * Which sample layers of the current kit the sampler panel would draw empty,
 * and the waveform loader's counters. The cache and the loader are the
 * sampler's (an app), so the cache lookup comes in as a function and the
 * counters are copied from whatever struct the loader reports them in.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad::control {

struct WaveStatus {
    static constexpr size_t kMissingChars = 512;

    int  kit    = -1;   ///< current kit id; -1 without a kit manager
    int  total  = 0;    ///< sample layers the current kit names
    int  cached = 0;    ///< ... whose waveform is in the cache
    /// "pad:layer,..." of the layers that are not, as far as kMissingChars
    /// holds; empty when none is missing.
    char missing[kMissingChars] = {};

    bool     loaderRunning = false;
    uint32_t requested  = 0;
    uint32_t dropped    = 0;
    uint32_t loaded     = 0;
    uint32_t retried    = 0;
    uint32_t failedInfo = 0;
    uint32_t failedRead = 0;
    uint32_t allocFail  = 0;
    uint32_t sweeps     = 0;
    uint32_t startFail  = 0;
    uint32_t queued     = 0;
};

/// The current kit's coverage: kit, total, cached, missing. @p isCached is
/// asked for a path as the kit names it when absolute, else beside the kit
/// and in its SAMPLES folder -- a lookup only, never the card.
void readWaveCoverage(WaveStatus& out, bool (*isCached)(const char* path));

/// The loader's side, from its own stats struct (same field names).
template <class LoaderStats>
void setWaveLoader(WaveStatus& out, const LoaderStats& st, bool running) {
    out.loaderRunning = running;
    out.requested  = st.requested;
    out.dropped    = st.dropped;
    out.loaded     = st.loaded;
    out.retried    = st.retried;
    out.failedInfo = st.failedInfo;
    out.failedRead = st.failedRead;
    out.allocFail  = st.allocFail;
    out.sweeps     = st.sweeps;
    out.startFail  = st.startFail;
    out.queued     = st.queued;
}

} // namespace crosspad::control
