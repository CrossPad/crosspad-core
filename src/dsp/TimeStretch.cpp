// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/dsp/TimeStretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace crosspad {

namespace {

constexpr uint32_t kFrame = TimeStretch::kFrame;
constexpr uint32_t kHop   = TimeStretch::kHop;
constexpr uint32_t kSeek  = TimeStretch::kSeek;
constexpr uint32_t kDecim = TimeStretch::kDecim;
/// What the search compares: a window's first hop, which overlaps the tail
/// of the one laid before it.
constexpr uint32_t kCorr  = kHop;
/// The input held at once: every candidate window, the natural continuation
/// of the last one, and the analysis hop between them at the largest ratio.
constexpr uint32_t kWinCap = 2 * kHop + 2 * kSeek + kFrame + kCorr + 64;
constexpr uint32_t kDecCap = (2 * kSeek + kCorr) / kDecim + 2;

constexpr double kPi = 3.14159265358979323846;

/// The working memory, carved out of what the caller gave.
struct Work {
    int16_t* win;    ///< kWinCap stereo frames of input
    float*   acc;    ///< kFrame stereo frames being overlap-added
    float*   hann;   ///< kFrame
    int16_t* out;    ///< kHop stereo frames on their way out
    float*   dec;    ///< kDecCap: the candidates' mono, decimated
    float*   nat;    ///< kCorr / kDecim: the natural continuation's, decimated
    float*   natFull;///< kCorr: and at full rate, for the fine search
};

size_t layout(void* base, Work* w)
{
    uint8_t* p = static_cast<uint8_t*>(base);
    size_t at = 0;
    auto take = [&](size_t bytes) {
        at = (at + 15) & ~size_t(15);
        uint8_t* r = p ? p + at : nullptr;
        at += bytes;
        return r;
    };
    uint8_t* win  = take(kWinCap * 2 * sizeof(int16_t));
    uint8_t* acc  = take(kFrame * 2 * sizeof(float));
    uint8_t* hann = take(kFrame * sizeof(float));
    uint8_t* out  = take(kHop * 2 * sizeof(int16_t));
    uint8_t* dec  = take(kDecCap * sizeof(float));
    uint8_t* nat  = take((kCorr / kDecim) * sizeof(float));
    uint8_t* natF = take(kCorr * sizeof(float));
    if (w) {
        w->win = reinterpret_cast<int16_t*>(win);
        w->acc = reinterpret_cast<float*>(acc);
        w->hann = reinterpret_cast<float*>(hann);
        w->out = reinterpret_cast<int16_t*>(out);
        w->dec = reinterpret_cast<float*>(dec);
        w->nat = reinterpret_cast<float*>(nat);
        w->natFull = reinterpret_cast<float*>(natF);
    }
    return at;
}

/// The input as a window that slides forward: what has been read stays until
/// the run has gone past it, so neighbouring windows cost one read of what is
/// new rather than one of all of it.
struct Input {
    int16_t* buf;
    int64_t  start = 0;
    uint32_t len = 0;
    uint32_t total;
    TimeStretch::Read read;
    void* ctx;

    /// [a, b) in the buffer; frames outside [0, total) are silence.
    bool ensure(int64_t a, int64_t b) {
        if (a >= start && b <= start + (int64_t)len) return true;
        if (a >= start && a < start + (int64_t)len) {
            const uint32_t keep = (uint32_t)(start + len - a);
            std::memmove(buf, buf + (a - start) * 2, (size_t)keep * 4);
            start = a;
            len = keep;
        } else {
            start = a;
            len = 0;
        }
        int64_t from = start + len;
        while (from < b) {
            int16_t* dst = buf + (from - start) * 2;
            if (from < 0) {
                const uint32_t n = (uint32_t)(std::min<int64_t>(b, 0) - from);
                std::memset(dst, 0, (size_t)n * 4);
                from += n;
            } else if (from >= (int64_t)total) {
                std::memset(dst, 0, (size_t)(b - from) * 4);
                from = b;
            } else {
                const uint32_t n = (uint32_t)(std::min<int64_t>(b, total) - from);
                if (!read(ctx, (uint32_t)from, dst, n)) return false;
                from += n;
            }
        }
        len = (uint32_t)(b - start);
        return true;
    }
    const int16_t* at(int64_t pos) const { return buf + (pos - start) * 2; }
    float mono(int64_t pos) const {
        const int16_t* s = at(pos);
        return (float)(s[0] + s[1]) * 0.5f;
    }
};

/// Where in [lo, hi] a window starts that continues @p natPos most alike:
/// normalised correlation over kCorr frames, coarse on decimated mono, then
/// to the frame around the best.
int64_t best_start(const Input& in, const Work& w, int64_t lo, int64_t hi, int64_t natPos)
{
    for (uint32_t j = 0; j < kCorr / kDecim; ++j) {
        float s = 0.0f;
        for (uint32_t d = 0; d < kDecim; ++d) s += in.mono(natPos + j * kDecim + d);
        w.nat[j] = s;
    }
    for (uint32_t j = 0; j < kCorr; ++j) w.natFull[j] = in.mono(natPos + j);

    const uint32_t decN = (uint32_t)((hi - lo + kCorr) / kDecim) + 1;
    for (uint32_t i = 0; i < decN && i < kDecCap; ++i) {
        float s = 0.0f;
        for (uint32_t d = 0; d < kDecim; ++d) {
            const int64_t pos = lo + (int64_t)i * kDecim + d;
            if (pos < in.start + (int64_t)in.len) s += in.mono(pos);
        }
        w.dec[i] = s;
    }
    int64_t best = lo;
    float bestScore = -1e30f;
    const uint32_t m = kCorr / kDecim;
    for (int64_t c = lo, i = 0; c <= hi && (uint32_t)i + m <= kDecCap; c += kDecim, ++i) {
        float dot = 0.0f, energy = 1e-6f;
        for (uint32_t j = 0; j < m; ++j) {
            const float v = w.dec[i + j];
            dot += v * w.nat[j];
            energy += v * v;
        }
        const float score = dot / std::sqrt(energy);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    const int64_t fineLo = best - (int64_t)kDecim + 1 > lo ? best - (int64_t)kDecim + 1 : lo;
    const int64_t fineHi = best + (int64_t)kDecim - 1 < hi ? best + (int64_t)kDecim - 1 : hi;
    bestScore = -1e30f;
    for (int64_t c = fineLo; c <= fineHi; ++c) {
        float dot = 0.0f, energy = 1e-6f;
        for (uint32_t j = 0; j < kCorr; ++j) {
            const float v = in.mono(c + j);
            dot += v * w.natFull[j];
            energy += v * v;
        }
        const float score = dot / std::sqrt(energy);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

/// Catmull-Rom over four taps; @p y1 is the frame at the integer position.
float hermite(float y0, float y1, float y2, float y3, float f)
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

int16_t to16(float v)
{
    const float r = std::round(v);
    return (int16_t)(r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r));
}

} // namespace

size_t TimeStretch::workBytes() { return layout(nullptr, nullptr); }

bool TimeStretch::run(uint32_t inFrames, uint32_t outFrames, void* work,
                      Read read, Write write, Keep keep, void* ctx)
{
    if (!work || !read || !write || !inFrames || !outFrames) return false;
    const double ratio = (double)outFrames / (double)inFrames;
    if (ratio < kMinRatio || ratio > kMaxRatio) return false;

    Work w;
    layout(work, &w);
    for (uint32_t n = 0; n < kFrame; ++n) w.hann[n] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * n / kFrame));
    std::memset(w.acc, 0, kFrame * 2 * sizeof(float));
    Input in{w.win, 0, 0, inFrames, read, ctx};

    /* The window before the first, whose falling half the first one's rising
     * half sums with: the first hop comes out exactly as it went in. */
    if (!in.ensure(0, kFrame)) return false;
    for (uint32_t n = 0; n < kHop; ++n) {
        const int16_t* s = in.at(n);
        w.acc[n * 2]     += s[0] * w.hann[n + kHop];
        w.acc[n * 2 + 1] += s[1] * w.hann[n + kHop];
    }

    const double ha = (double)kHop / ratio;   // input frames per output hop
    int64_t prev = 0;
    uint32_t done = 0;
    for (uint32_t k = 0; done < outFrames; ++k) {
        int64_t pos = 0;
        if (k) {
            const int64_t tau = (int64_t)std::llround(k * ha);
            const int64_t nat = prev + kHop;
            const int64_t lo = tau - (int64_t)kSeek;
            const int64_t hi = tau + (int64_t)kSeek;
            const int64_t a = nat < lo ? nat : lo;
            const int64_t b = hi + (int64_t)kFrame;
            if (b - a > (int64_t)kWinCap || !in.ensure(a, b)) return false;
            pos = best_start(in, w, lo, hi, nat);
        }
        for (uint32_t n = 0; n < kFrame; ++n) {
            const int16_t* s = in.at(pos + n);
            w.acc[n * 2]     += s[0] * w.hann[n];
            w.acc[n * 2 + 1] += s[1] * w.hann[n];
        }
        const uint32_t n = outFrames - done < kHop ? outFrames - done : kHop;
        for (uint32_t i = 0; i < n * 2; ++i) w.out[i] = to16(w.acc[i]);
        if (!write(ctx, w.out, n)) return false;
        std::memmove(w.acc, w.acc + kHop * 2, (kFrame - kHop) * 2 * sizeof(float));
        std::memset(w.acc + (kFrame - kHop) * 2, 0, kHop * 2 * sizeof(float));
        done += n;
        prev = pos;
        if (keep && !keep(ctx, done, outFrames)) return false;
    }
    return true;
}

bool TimeStretch::repitch(uint32_t inFrames, uint32_t outFrames, void* work,
                          Read read, Write write, Keep keep, void* ctx)
{
    if (!work || !read || !write || !inFrames || !outFrames) return false;
    const double ratio = (double)outFrames / (double)inFrames;
    if (ratio < kMinRatio || ratio > kMaxRatio) return false;

    Work w;
    layout(work, &w);
    Input in{w.win, 0, 0, inFrames, read, ctx};

    const double step = (double)inFrames / (double)outFrames;   // input frames per output frame
    for (uint32_t done = 0; done < outFrames;) {
        const uint32_t n = outFrames - done < kHop ? outFrames - done : kHop;
        /* The four taps around every position this hop reads. */
        const int64_t a = (int64_t)std::floor(done * step) - 1;
        const int64_t b = (int64_t)std::floor((done + n - 1) * step) + 3;
        if (b - a > (int64_t)kWinCap || !in.ensure(a, b)) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const double pos = (double)(done + i) * step;
            const int64_t k = (int64_t)std::floor(pos);
            const float f = (float)(pos - (double)k);
            const int16_t* s = in.at(k - 1);
            w.out[i * 2]     = to16(hermite(s[0], s[2], s[4], s[6], f));
            w.out[i * 2 + 1] = to16(hermite(s[1], s[3], s[5], s[7], f));
        }
        if (!write(ctx, w.out, n)) return false;
        done += n;
        if (keep && !keep(ctx, done, outFrames)) return false;
    }
    return true;
}

} // namespace crosspad
