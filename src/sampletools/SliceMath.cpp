// SPDX-License-Identifier: MIT
#include "crosspad/sampletools/SliceMath.hpp"

#include <algorithm>
#include <cmath>

namespace crosspad {
namespace SliceMath {

uint8_t envelopeStep(float meanSquare)
{
    if (meanSquare <= 0.0f) return 0;
    const float db = 10.0f * std::log10(meanSquare);          // 0 dB = full scale
    const float step = (db + kEnvRangeDb) / kEnvDbPerStep;
    return (uint8_t)std::max(0.0f, std::min(255.0f, step));
}

std::vector<uint32_t> onsets(const std::vector<uint8_t>& envelope, uint32_t hop, uint32_t envStart,
                             uint32_t start, uint32_t end, int sensitivity, uint32_t minGapFrames,
                             uint8_t maxSlices)
{
    std::vector<uint32_t> out;
    if (end <= start) return out;
    out.push_back(start);
    if (!hop || envelope.size() < 3 || maxSlices < 2) return out;

    sensitivity = std::max(0, std::min(100, sensitivity));
    const float riseDb = 30.0f - 27.0f * (float)sensitivity / 100.0f;
    const int   rise   = std::max(1, (int)std::lround(riseDb / kEnvDbPerStep));
    /* A hit rises out of something: a rise that stays 50 dB under the
     * loudest part of the loop is noise getting louder, not a drum. */
    const uint8_t peak  = *std::max_element(envelope.begin(), envelope.end());
    const int     floor = (int)peak - (int)std::lround(50.0f / kEnvDbPerStep);

    struct Hit { uint32_t at; int strength; };
    std::vector<Hit> hits;
    const size_t first = start > envStart ? (start - envStart) / hop + 1 : 1;
    const size_t last  = std::min(envelope.size() - 1, (size_t)((end - envStart) / hop));
    for (size_t i = first; i < last; ++i) {
        const int d  = (int)envelope[i] - (int)envelope[i - 1];
        const int dn = (int)envelope[i + 1] - (int)envelope[i];
        if (d < rise || (int)envelope[i] < floor) continue;
        if (dn > d) continue;                       // the rise goes on: the next hop is the hit
        /* The attack began in the hop before the one that shows it. */
        const uint32_t at = envStart + (uint32_t)(i - 1) * hop;
        if (at <= start || at >= end) continue;
        hits.push_back({at, d});
    }

    /* Too close together: the stronger of the two stays. */
    std::vector<Hit> spaced;
    for (const Hit& h : hits) {
        const uint32_t prev = spaced.empty() ? start : spaced.back().at;
        if (h.at - prev >= minGapFrames) spaced.push_back(h);
        else if (!spaced.empty() && h.strength > spaced.back().strength) spaced.back() = h;
    }
    if (spaced.size() > (size_t)(maxSlices - 1)) {
        std::sort(spaced.begin(), spaced.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        spaced.resize(maxSlices - 1);
        std::sort(spaced.begin(), spaced.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });
    }
    for (const Hit& h : spaced) out.push_back(h.at);
    return out;
}

std::vector<uint32_t> equal(uint32_t start, uint32_t end, uint8_t count)
{
    std::vector<uint32_t> out;
    if (end <= start || count == 0) return out;
    count = std::min(count, kMaxSlices);
    const uint64_t len = end - start;
    for (uint8_t i = 0; i < count; ++i) out.push_back(start + (uint32_t)(len * i / count));
    return out;
}

float bpmOf(uint32_t frames, uint32_t sampleRate, uint8_t bars)
{
    if (!frames || !sampleRate || !bars) return 0.0f;
    const float seconds = (float)frames / (float)sampleRate;
    return 60.0f * 4.0f * (float)bars / seconds;
}

uint32_t attackFrames(uint16_t attackMs, uint32_t sampleRate)
{
    const uint32_t ms = std::min<uint32_t>(attackMs, kMaxAttackMs);
    return std::max(kMinFadeInFrames, (uint32_t)((uint64_t)ms * sampleRate / 1000u));
}

float sliceGain(uint32_t rel, uint32_t len, uint32_t attack)
{
    if (rel >= len) return 0.0f;
    float g = 1.0f;
    if (rel < attack) g = (float)rel / (float)attack;
    if (len - rel <= kFadeOutFrames) g = std::min(g, (float)(len - rel - 1) / (float)kFadeOutFrames);
    return g;
}

double pitchRatio(int semitones)
{
    semitones = std::max(-kMaxSemitones, std::min(kMaxSemitones, semitones));
    return std::pow(2.0, (double)semitones / 12.0);
}

uint32_t pitchedFrames(uint32_t srcFrames, double ratio)
{
    if (ratio <= 0.0) return srcFrames;
    return (uint32_t)std::ceil((double)srcFrames / ratio - 1e-9);
}

void readPitched(const int16_t* src, uint32_t srcFrames, double pos, double ratio,
                 int16_t* out, uint32_t n)
{
    if (!srcFrames) {
        std::fill(out, out + n * 2, (int16_t)0);
        return;
    }
    const int64_t last = (int64_t)srcFrames - 1;
    auto tap = [&](int64_t i, int ch) -> float {
        return (float)src[std::max<int64_t>(0, std::min(last, i)) * 2 + ch];
    };
    for (uint32_t k = 0; k < n; ++k) {
        const double p = pos + (double)k * ratio;
        const int64_t i = (int64_t)std::floor(p);
        const float f = (float)(p - (double)i);
        for (int ch = 0; ch < 2; ++ch) {
            const float y0 = tap(i - 1, ch), y1 = tap(i, ch), y2 = tap(i + 1, ch), y3 = tap(i + 2, ch);
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            const float y = ((c3 * f + c2) * f + c1) * f + y1;
            out[k * 2 + ch] = (int16_t)std::max(-32768.0f, std::min(32767.0f, std::round(y)));
        }
    }
}

} // namespace SliceMath
} // namespace crosspad
