// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/sampletools/SliceMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

TrimPoints autoTrim(const std::vector<uint8_t>& envelope, uint32_t hop, uint32_t envStart,
                    uint32_t frames, uint32_t sampleRate)
{
    TrimPoints out{0, frames};
    if (!hop || !frames || envelope.size() < 4) return out;

    /* The floor, from a histogram rather than a sort: the envelope is one
     * byte per hop and there are 256 of them, so counting is both exact and
     * free of an allocation on a board whose internal RAM this runs in. */
    uint32_t count[256] = {};
    for (uint8_t v : envelope) ++count[v];
    const size_t want = std::max<size_t>(1, envelope.size() * (size_t)kTrimFloorPct / 100);
    size_t seen = 0;
    int    floorStep = 0;
    for (int v = 0; v < 256; ++v) {
        seen += count[v];
        if (seen >= want) { floorStep = v; break; }
    }
    const int peak = (int)*std::max_element(envelope.begin(), envelope.end());

    /* Nothing to trim: a take recorded hot all the way through, or one that
     * never rose out of the room. Cutting either would cut music. */
    const int span = (int)std::lround(kTrimSpanDb / kEnvDbPerStep);
    if (peak - floorStep < span) return out;

    /* The gate goes in the gap between the room and the sound, and it is found
     * rather than guessed. Counting up from the floor does not work on a
     * microphone take: a room is not steady, and in one measured here it
     * spread 10 dB above its own quietest hop, so floor + 9 dB was still room
     * and the start landed in it.
     *
     * So the threshold that best separates the two is taken straight from the
     * histogram already built above -- Otsu's: for every candidate, weigh the
     * two classes it makes by how far apart their means are, and keep the
     * candidate where that is largest. It needs no second pass over anything
     * and no allocation. On its own it would split a take that has no silence
     * at all, since it always answers; the span test above is what refuses
     * those, which is why that line had to be measured rather than assumed. */
    uint64_t total = 0, sum = 0;
    for (int v = 0; v < 256; ++v) { total += count[v]; sum += (uint64_t)v * count[v]; }
    uint64_t wB = 0, sumB = 0;
    double   bestVar = -1.0;
    int      split = floorStep;
    for (int v = 0; v < 256; ++v) {
        wB   += count[v];
        sumB += (uint64_t)v * count[v];
        if (!wB) continue;
        const uint64_t wF = total - wB;
        if (!wF) break;
        const double mB = (double)sumB / (double)wB;
        const double mF = (double)(sum - sumB) / (double)wF;
        const double var = (double)wB * (double)wF * (mB - mF) * (mB - mF);
        if (var > bestVar) { bestVar = var; split = v; }
    }
    /* Never below the floor plus a little: a histogram with one huge silent
     * bucket can put the split inside it. */
    const int gate = std::max(split + 1,
                              floorStep + (int)std::lround(kTrimGateDb / kEnvDbPerStep));

    size_t firstAbove = envelope.size();
    for (size_t i = 0; i < envelope.size(); ++i) {
        if ((int)envelope[i] >= gate) { firstAbove = i; break; }
    }
    if (firstAbove == envelope.size()) return out;      // nothing cleared it

    /* The hit, not the sound before it. A sample is going under a pad: what
     * must land the instant the pad is struck is the transient, and a take
     * holds plenty that clears a gate without being one -- a breath, a chair,
     * a hand on the case. So a transient is looked for first, and it has to
     * be a jump of kTrimRiseDb between one hop and the next that also lands
     * clear of the gate: a fade-up fails the first test, the room's own wobble
     * the second.
     *
     * The search starts at the gate crossing and not before it, so the floor's
     * own wobble cannot pass for an attack. */
    const int rise    = (int)std::lround(kTrimRiseDb / kEnvDbPerStep);
    /* Measured from the gate, not from the peak: a take can hold a hi-hat and
     * a kick 30 dB louder, and anchoring this to the loudest thing in the file
     * made the hat fail the test and the start jump forward to the kick. */
    const int loudest = gate + rise;
    const int skip    = (int)std::lround(kTrimSkipDb / kEnvDbPerStep);
    size_t at  = firstAbove;
    int    pre = 0;          // the loudest hop between the gate and the candidate
    for (size_t i = (firstAbove ? firstAbove : 1); i < envelope.size(); ++i) {
        const int v = (int)envelope[i];
        /* Three tests, and the third is the one that keeps a swell whole: what
         * lies between the gate and this hop has to be negligible beside it.
         * A breath 40 dB under the hit is; the first stage of a note fading up
         * is not, and moving the start past it would cut the note. */
        if (v - (int)envelope[i - 1] >= rise && v >= loudest && v - pre >= skip) {
            at = i;
            break;
        }
        /* The body of the take has arrived without a transient to find -- a
         * bowed note, a pad swelling in. Where it cleared the gate is where it
         * begins, and looking further can only land later than that. */
        if (v >= loudest) break;
        pre = std::max(pre, v);
    }

    /* One hop back and no further: the attack began in the hop before the one
     * that shows it, because that hop still averages mostly what came before.
     * Going back by any rule that reads the level instead walks through a
     * living room hop by hop -- which is how this put START a fifth of a
     * second before the hit. No lead on top either: with a hop of ~5 ms, that
     * single step is already as much silence as a pad should ever carry. */
    const size_t foot = at ? at - 1 : 0;

    size_t last = foot;
    for (size_t i = envelope.size(); i-- > foot;) {
        if ((int)envelope[i] >= gate) { last = i; break; }
    }
    /* A tail drops under the gate while it is still sounding, so it is followed
     * -- but only just under, since the gate sits in the gap and a few dB below
     * it is still clear of the room. Measured from the floor instead, this
     * walked into a live room and carried the end of the loop to the end of the
     * take. */
    const int tailGate = gate - (int)std::lround(kTrimTailDb / kEnvDbPerStep);
    while (last + 1 < envelope.size() && (int)envelope[last + 1] > tailGate) ++last;

    const uint32_t release = sampleRate ? (uint32_t)sampleRate * kTrimTailMs / 1000 : 0;
    const uint64_t a = (uint64_t)envStart + (uint64_t)foot * hop;
    const uint64_t b = (uint64_t)envStart + ((uint64_t)last + 1) * hop + release;

    out.start = (uint32_t)std::min<uint64_t>(a, frames);
    out.end   = (uint32_t)std::min<uint64_t>(b, frames);
    /* Never hand back an empty or inverted loop, whatever the envelope said. */
    if (out.end <= out.start) { out.start = 0; out.end = frames; }
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

uint32_t zeroCrossing(const int16_t* stereo, uint32_t n, uint32_t at)
{
    if (!stereo || at >= n) return at;
    auto mono = [stereo](uint32_t i) { return (int32_t)stereo[i * 2] + (int32_t)stereo[i * 2 + 1]; };
    /* Frame i is a crossing when it and i-1 lie on different sides of zero,
     * or it is zero; the one of the pair nearer zero is the cut. */
    auto pick = [&](uint32_t i) {
        const int32_t a = mono(i - 1), b = mono(i);
        return std::abs(a) < std::abs(b) ? i - 1 : i;
    };
    if (mono(at) == 0) return at;
    /* From the pair after @p at: a cut already on a crossing finds the same
     * one again, so snapping twice moves nothing. */
    for (uint32_t i = std::min(at + 1, n - 1); i >= 1; --i) {
        if (mono(i) == 0) return i;
        if ((mono(i - 1) < 0) != (mono(i) < 0)) return pick(i);
    }
    for (uint32_t i = at + 2; i < n; ++i) {
        if (mono(i) == 0) return i;
        if ((mono(i - 1) < 0) != (mono(i) < 0)) return pick(i);
    }
    return at;
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
