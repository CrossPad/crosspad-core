// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace crosspad {

/// Offline time-stretch of 16-bit stereo: the length changes, the pitch and
/// the timbre do not. WSOLA -- windows of the input laid down at a steady
/// output hop, each moved up to kSeek frames from where the ratio puts it to
/// the place that continues the previous one most alike, so a beat is not
/// smeared by windows that disagree about its phase.
///
/// Streams both ways through callbacks, so a clip of minutes needs no more
/// than workBytes() of memory -- the caller allocates it (PSRAM on the board).
/// Good for beats and loops within about +-20 %; the ratio is held to
/// [kMinRatio, kMaxRatio].
///
/// repitch() is the other way to fit a clip: varispeed, as a turntable -- the
/// clip is read faster or slower, so its pitch moves with its length (half
/// the length, an octave up). Same callbacks, same memory, same ratios.
class TimeStretch {
public:
    /// Stereo frames [frame, frame + n) of the input into @p dst; whatever is
    /// past the input's end (or before its start) is the caller's to zero.
    /// False gives up the run (a card error).
    using Read = bool (*)(void* ctx, uint32_t frame, int16_t* dst, uint32_t n);
    /// @p n stereo frames of output, in order. False gives up the run.
    using Write = bool (*)(void* ctx, const int16_t* src, uint32_t n);
    /// Between output hops: false stops the run, something else needs the
    /// thread. May be null.
    using Keep = bool (*)(void* ctx, uint32_t done, uint32_t total);

    static constexpr uint32_t kFrame = 1024;        ///< window, frames
    static constexpr uint32_t kHop   = kFrame / 2;  ///< output hop: Hann at 50 % sums to one
    static constexpr uint32_t kSeek  = 256;         ///< how far a window may move to line up
    static constexpr uint32_t kDecim = 4;           ///< the coarse search's decimation
    static constexpr float    kMinRatio = 0.5f;     ///< output / input
    static constexpr float    kMaxRatio = 2.0f;

    /// Bytes of working memory run() needs.
    static size_t workBytes();

    /// @p inFrames of input into exactly @p outFrames of output. The first
    /// kHop frames come out as they went in, so a clip starts on its own
    /// attack. False: a ratio out of range, a read or write that failed, or
    /// keep() asked to stop.
    static bool run(uint32_t inFrames, uint32_t outFrames, void* work,
                    Read read, Write write, Keep keep, void* ctx);

    /// @p inFrames into exactly @p outFrames, the pitch following: cubic
    /// Hermite between the input's frames, as the pads pitch a sample, and
    /// not band-limited -- read faster, what was above the new Nyquist folds
    /// back, as it does on a pitched pad. The first frame comes out as it went
    /// in. False as run().
    static bool repitch(uint32_t inFrames, uint32_t outFrames, void* work,
                        Read read, Write write, Keep keep, void* ctx);
};

} // namespace crosspad
