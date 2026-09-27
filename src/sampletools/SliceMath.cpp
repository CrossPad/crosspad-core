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

} // namespace SliceMath
} // namespace crosspad
