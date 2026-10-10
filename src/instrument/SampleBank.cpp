// SPDX-License-Identifier: GPL-3.0-or-later
#include "crosspad/instrument/SampleBank.hpp"

#include <cmath>

namespace crosspad {

namespace {
const Zone kEmptyZone{};
}

SampleBank::SampleBank(void* (*alloc)(size_t), void (*free)(void*),
                       MemoryGenerationFn generation)
    : alloc_(alloc), free_(free), genFn_(generation),
      generation_(generation ? generation() : 0) {}

SampleBank::~SampleBank() {
    // A stale bank's blocks went with the wipe that made it stale: handing
    // them to free() would corrupt the heap that replaced them.
    if (free_ && !stale()) {
        for (uint8_t i = 0; i < blockCount_; ++i) {
            if (blocks_[i]) free_(blocks_[i]);
        }
    }
    blockCount_ = 0;
    zoneCount_ = 0;
    bytes_ = 0;
}

int16_t* SampleBank::allocPcm(uint32_t length) {
    if (!alloc_ || length == 0 || blockCount_ >= kMaxZones || stale()) return nullptr;
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

bool SampleBank::editZone(uint8_t i, float rootMidi, uint32_t loopStart, uint32_t loopEnd) {
    if (i >= zoneCount_) return false;
    if (!loopValid(zones_[i].length, loopStart, loopEnd)) return false;
    if (loopEnd <= loopStart) { loopStart = 0; loopEnd = 0; }
    zones_[i].rootMidi  = rootMidi;
    zones_[i].loopStart = loopStart;
    zones_[i].loopEnd   = loopEnd;
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
