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

bool SampleBank::addZone(const Zone& z) {
    if (zoneCount_ >= kMaxZones || !z.pcm || z.length == 0) return false;
    bool owned = false;
    for (uint8_t i = 0; i < blockCount_; ++i) {
        if (blocks_[i] == z.pcm) { owned = true; break; }
    }
    if (!owned) return false;

    Zone copy = z;
    if (copy.loopEnd <= copy.loopStart) {
        copy.loopStart = 0;
        copy.loopEnd = 0;
    } else {
        if (copy.loopEnd > copy.length) return false;
        if (copy.loopEnd - copy.loopStart < kMinLoopSamples) return false;
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
