// SPDX-License-Identifier: MIT
#include "crosspad/instrument/SampleBank.hpp"

#include <cmath>

namespace crosspad {

namespace {
const Zone kEmptyZone{};
}

SampleBank::SampleBank(void* (*alloc)(size_t), void (*free)(void*))
    : alloc_(alloc), free_(free) {}

SampleBank::~SampleBank() {
    if (free_) {
        for (uint8_t i = 0; i < blockCount_; ++i) {
            if (blocks_[i]) free_(blocks_[i]);
        }
    }
    blockCount_ = 0;
    zoneCount_ = 0;
    bytes_ = 0;
}

int16_t* SampleBank::allocPcm(uint32_t length) {
    if (!alloc_ || length == 0 || blockCount_ >= kMaxZones) return nullptr;
    const size_t n = static_cast<size_t>(length) * sizeof(int16_t);
    int16_t* p = static_cast<int16_t*>(alloc_(n));
    if (!p) return nullptr;
    blocks_[blockCount_++] = p;
    bytes_ += n;
    return p;
}

bool SampleBank::loopValid(uint32_t length, uint32_t loopStart, uint32_t loopEnd) {
    if (loopEnd <= loopStart) return true;          // one-shot
    if (loopEnd > length) return false;
    return (loopEnd - loopStart) >= kMinLoopSamples;
}

uint32_t SampleBank::loopXfadeFor(uint32_t loopStart, uint32_t loopEnd, uint32_t xfade) {
    if (loopEnd <= loopStart) return 0;
    const uint32_t span = loopEnd - loopStart;
    if (xfade > span) xfade = span;
    if (xfade > loopStart) xfade = loopStart;
    return xfade;
}

uint32_t SampleBank::startFor(uint32_t length, uint32_t loopStart, uint32_t loopEnd, uint32_t start) {
    if (start >= length) return 0;
    if (loopEnd > loopStart && start >= loopEnd) return loopStart;
    return start;
}

bool SampleBank::editZone(uint8_t i, float rootMidi, uint32_t loopStart, uint32_t loopEnd,
                          uint32_t loopXfade, uint32_t start) {
    if (i >= zoneCount_) return false;
    if (!loopValid(zones_[i].length, loopStart, loopEnd)) return false;
    if (loopEnd <= loopStart) { loopStart = 0; loopEnd = 0; }
    if (loopXfade == kKeepXfade) loopXfade = zones_[i].loopXfade;
    if (start == kKeepStart) start = zones_[i].start;
    zones_[i].rootMidi  = rootMidi;
    zones_[i].loopStart = loopStart;
    zones_[i].loopEnd   = loopEnd;
    zones_[i].loopXfade = loopXfadeFor(loopStart, loopEnd, loopXfade);
    zones_[i].start     = startFor(zones_[i].length, loopStart, loopEnd, start);
    return true;
}

bool SampleBank::addZone(const Zone& z) {
    if (zoneCount_ >= kMaxZones || !z.pcm || z.length == 0) return false;
    bool owned = false;
    for (uint8_t i = 0; i < blockCount_; ++i) {
        if (blocks_[i] == z.pcm) { owned = true; break; }
    }
    if (!owned) return false;

    Zone copy = z;
    if (!loopValid(copy.length, copy.loopStart, copy.loopEnd)) return false;
    if (copy.loopEnd <= copy.loopStart) {
        copy.loopStart = 0;
        copy.loopEnd = 0;
    }
    copy.loopXfade = loopXfadeFor(copy.loopStart, copy.loopEnd, copy.loopXfade);
    copy.start     = startFor(copy.length, copy.loopStart, copy.loopEnd, copy.start);
    zones_[zoneCount_++] = copy;
    return true;
}

const Zone& SampleBank::zone(uint8_t i) const {
    return (i < zoneCount_) ? zones_[i] : kEmptyZone;
}

const Zone* SampleBank::nearestZone(float midi) const {
    if (zoneCount_ == 0) return nullptr;
    const Zone* best = &zones_[0];
    float bestDist = std::fabs(zones_[0].rootMidi - midi);
    for (uint8_t i = 1; i < zoneCount_; ++i) {
        const float d = std::fabs(zones_[i].rootMidi - midi);
        // Ties go to the lower root: only take the new zone on a strict win,
        // or on an equal distance with a lower root.
        if (d < bestDist || (d == bestDist && zones_[i].rootMidi < best->rootMidi)) {
            best = &zones_[i];
            bestDist = d;
        }
    }
    return best;
}

} // namespace crosspad
