// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file MixerLevels.hpp
 * @brief The mixer meter verb (board MIX_LVL, simulator mix_lvl), gathered once.
 *
 * Per-channel and per-output peaks with each channel's route, volume, trim and
 * mute, and each output's limiter and ceiling: where a rendered voice is lost
 * before the output bus. The mixer engine is an app (crosspad-mixer), not
 * core, so the gather is a template over its read API rather than a call core
 * could link.
 */

#include <cstdint>

namespace crosspad::control {

struct MixerChannelLevel {
    uint8_t     ch      = 0;
    const char* name    = nullptr;   ///< the mixer's own string, as it returned it
    float       peakL   = 0.0f;
    float       peakR   = 0.0f;
    float       volume  = 0.0f;
    float       trim    = 0.0f;      ///< an Arrange seq track's fader, while the song plays
    bool        muted   = false;
    uint8_t     outputs = 0;         ///< outputs the route mask covers
    uint32_t    routes  = 0;         ///< bit o set: routed to output o
};

struct MixerOutputLevel {
    uint8_t out     = 0;
    float   peakL   = 0.0f;
    float   peakR   = 0.0f;
    float   volume  = 0.0f;
    bool    muted   = false;
    float   limit   = 1.0f;   ///< the limiter's deepest gain since the last read; 1 = it never acted
    float   ceiling = 1.0f;   ///< where the limiter holds the output
};

/// The pads tap bounces and takes record from: its limiter, read as an
/// output's is.
struct MixerTapLevel {
    float limit = 1.0f;
};

/// The route mask's width: outputs past it are not reported.
constexpr uint8_t kMixerRouteBits = 32;

/// Every active channel, in channel order, then every output, then the pads
/// tap: one row at a time to @p onChannel / @p onOutput / @p onTap, so a
/// caller can send a row before the next is read and nothing holds the whole
/// desk. The limiter readings are taken: the next read starts again at 1.
template <class Mixer, class OnChannel, class OnOutput, class OnTap>
void readMixerLevels(Mixer& mx, OnChannel&& onChannel, OnOutput&& onOutput, OnTap&& onTap) {
    const uint8_t nout = mx.numOutputs();
    for (uint8_t ch = 0; ch < mx.maxChannels(); ++ch) {
        if (!mx.isChannelActive(ch)) continue;
        MixerChannelLevel row;
        row.ch = ch;
        mx.getChannelLevel(ch, row.peakL, row.peakR);
        row.outputs = nout < kMixerRouteBits ? nout : kMixerRouteBits;
        for (uint8_t o = 0; o < row.outputs; ++o) {
            if (mx.isRouteEnabled(ch, o)) row.routes |= 1u << o;
        }
        row.name   = mx.getChannelName(ch);
        row.volume = mx.getChannelVolume(ch);
        row.trim   = mx.getChannelTrim(ch);
        row.muted  = mx.isChannelMuted(ch);
        onChannel(row);
    }
    for (uint8_t o = 0; o < nout; ++o) {
        MixerOutputLevel row;
        row.out = o;
        mx.getOutputLevel(o, row.peakL, row.peakR);
        row.volume  = mx.getOutputVolume(o);
        row.muted   = mx.isOutputMuted(o);
        row.limit   = mx.takeOutputLimit(o);
        row.ceiling = mx.getOutputCeiling(o);
        onOutput(row);
    }
    MixerTapLevel tap;
    tap.limit = mx.takeOutputLimit(Mixer::kTapLimit);
    onTap(tap);
}

/// The same, for a caller that does not report the pads tap.
template <class Mixer, class OnChannel, class OnOutput>
void readMixerLevels(Mixer& mx, OnChannel&& onChannel, OnOutput&& onOutput) {
    readMixerLevels(mx, onChannel, onOutput, [](const MixerTapLevel&) {});
}

} // namespace crosspad::control
