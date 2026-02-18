#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifndef KIT_LOADER_MAX_SAMPLES_PER_PAD
#define KIT_LOADER_MAX_SAMPLES_PER_PAD 8
#endif

namespace crosspad {

class SampleSlot {
public:
    uint8_t slot = 0;
    std::string sample = "";

    SampleSlot(uint8_t s, std::string f) : slot(s), sample(f) {}
    SampleSlot(uint8_t s) : slot(s) { sample = std::to_string(s) + ".wav"; }

    bool isEmpty() { return (slot == 0) || !sample.ends_with(".wav"); }
};

class Pad {
public:
    uint8_t number = 0;
    uint32_t color = 0x20FF10;
    uint32_t color_pressed = 0xFF1020;
    int choke_group = 0;
    bool cycle = false;
    int pitch = 0;
    int volume = 100;
    int pan = 0;
    bool dirty = false;
    uint8_t note = 0;

    std::vector<SampleSlot> samples;
    uint8_t sampleCount = 0;

    Pad() {}
    Pad(uint8_t n) : number(n), note(n) {}
    bool IsModified() { return dirty; }

    void AddDefaultSample() {
        if (sampleCount >= KIT_LOADER_MAX_SAMPLES_PER_PAD) return;
        samples.push_back(SampleSlot(++sampleCount));
    }

    bool AddSample(std::string sample, bool markDirty = true) {
        if (sampleCount >= KIT_LOADER_MAX_SAMPLES_PER_PAD) return false;
        samples.push_back(SampleSlot(++sampleCount, sample));
        if (markDirty) dirty = true;
        return true;
    }

    void RemoveSample(uint8_t index) {
        if (index < sampleCount) {
            dirty = true;
            samples.erase(samples.begin() + index);
            sampleCount--;
        } else if (sampleCount > 0) {
            dirty = true;
            samples.pop_back();
            sampleCount--;
        }
    }

    void RemoveAllSamples() {
        if (sampleCount > 0) dirty = true;
        samples.clear();
        sampleCount = 0;
    }

    void SetSample(uint8_t index, std::string sample) {
        if (index < sampleCount) {
            dirty = true;
            samples[index].sample = sample;
        }
    }

    void SwapSamples(uint8_t index1, uint8_t index2) {
        if (index1 < sampleCount && index2 < sampleCount) {
            dirty = true;
            std::swap(samples[index1], samples[index2]);
        }
    }

    uint8_t GetSampleCount() { return sampleCount; }
};

} // namespace crosspad
