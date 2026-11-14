// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file test_helpers.hpp
 * @brief Mocks, stubs, and fixtures for crosspad-core standalone tests.
 *
 * All mocks are self-contained — no platform dependencies.
 * Uses SyncEventBus (std::mutex) for event dispatch.
 */

#include "crosspad/platform/CrosspadPlatformInit.hpp"
#include "crosspad/platform/PlatformCapabilities.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/event/SyncEventBus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/midi/NullMidiOutput.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/platform/IClock.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/stm32/IStm32RegisterBus.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace test {

// ── Mock clock (controllable time for animation tests) ──

class MockClock : public crosspad::IClock {
public:
    int64_t getTimeUs() override { return timeUs_; }
    void advance(int64_t us) { timeUs_ += us; }
    void set(int64_t us) { timeUs_ = us; }
private:
    int64_t timeUs_ = 0;
};

// ── Mock LED strip (readable pixel buffer) ──

class MockLedStrip : public crosspad::ILedStrip {
public:
    void begin() override {}
    void setPixel(uint16_t idx, crosspad::RgbColor color) override {
        if (idx < 16) pixels[idx] = color;
    }
    void refresh() override { refreshCount++; }
    uint16_t getPixelCount() const override { return 16; }

    crosspad::RgbColor pixels[16] = {};
    int refreshCount = 0;
};

// ── Recording MIDI output (captures all sent messages) ──

class RecordingMidiOutput : public crosspad::IMidiOutput {
public:
    struct Msg {
        enum Type { NoteOn, NoteOff, CC, Aftertouch, ProgramChange, PitchBend } type;
        uint8_t note = 0;
        uint8_t velocity = 0;
        uint8_t channel = 0;
        uint8_t controller = 0;
        uint8_t value = 0;
        int16_t pitchBend = 0;
    };

    void sendNoteOn(uint8_t note, uint8_t channel) override {
        msgs.push_back({Msg::NoteOn, note, 127, channel});
    }
    void sendNoteOff(uint8_t note, uint8_t channel) override {
        msgs.push_back({Msg::NoteOff, note, 0, channel});
    }
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override {
        msgs.push_back({Msg::NoteOn, note, velocity, channel});
    }
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override {
        msgs.push_back({Msg::NoteOff, note, velocity, channel});
    }
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override {
        Msg m; m.type = Msg::CC; m.controller = controller; m.value = value; m.channel = channel;
        msgs.push_back(m);
    }
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override {
        Msg m; m.type = Msg::Aftertouch; m.note = note; m.value = pressure; m.channel = channel;
        msgs.push_back(m);
    }
    void sendProgramChange(uint8_t program, uint8_t channel) override {
        Msg m; m.type = Msg::ProgramChange; m.value = program; m.channel = channel;
        msgs.push_back(m);
    }
    void sendPitchBend(int16_t value, uint8_t channel) override {
        Msg m; m.type = Msg::PitchBend; m.pitchBend = value; m.channel = channel;
        msgs.push_back(m);
    }

    size_t count() const { return msgs.size(); }
    void clear() { msgs.clear(); }

    std::vector<Msg> msgs;
};

// ── In-memory key-value store ──

class MemoryKVStore : public crosspad::IKeyValueStore {
public:
    bool init() override { return true; }

    void saveBool(const char* ns, const char* key, bool value) override {
        store_[makeKey(ns, key)] = value ? 1 : 0;
    }
    void saveU8(const char* ns, const char* key, uint8_t value) override {
        store_[makeKey(ns, key)] = value;
    }
    void saveI32(const char* ns, const char* key, int32_t value) override {
        store_[makeKey(ns, key)] = value;
    }

    bool readBool(const char* ns, const char* key, bool defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? (it->second != 0) : defaultVal;
    }
    uint8_t readU8(const char* ns, const char* key, uint8_t defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? static_cast<uint8_t>(it->second) : defaultVal;
    }
    int32_t readI32(const char* ns, const char* key, int32_t defaultVal) override {
        auto it = store_.find(makeKey(ns, key));
        return it != store_.end() ? it->second : defaultVal;
    }

    void eraseAll() override { store_.clear(); }
    size_t size() const { return store_.size(); }

private:
    std::string makeKey(const char* ns, const char* key) {
        return std::string(ns) + "/" + key;
    }
    std::map<std::string, int32_t> store_;
};

// ── Mock STM32 register bus (records writes, serves canned reads) ──

class MockStm32Bus : public crosspad::IStm32RegisterBus {
public:
    struct Write { uint16_t reg; std::vector<uint8_t> data; };
    std::vector<Write> writes;
    std::map<uint16_t, uint8_t> readValues;  // reg -> byte
    bool failWrites = false;

    bool writeReg(uint16_t reg, const uint8_t* data, size_t len) override {
        if (failWrites) return false;
        writes.push_back({reg, std::vector<uint8_t>(data, data + len)});
        return true;
    }
    bool readReg(uint16_t reg, uint8_t* dst, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            auto it = readValues.find((uint16_t)(reg + i));
            dst[i] = (it != readValues.end()) ? it->second : 0;
        }
        return true;
    }

    // Helper: find the single-byte value written to reg (last wins), or -1.
    int lastByteAt(uint16_t reg) const {
        int v = -1;
        for (const auto& w : writes)
            if (w.reg == reg && w.data.size() >= 1) v = w.data[0];
        return v;
    }
};

// ── Full platform fixture (uses SyncEventBus, no FreeRTOS) ──

struct CoreTestPlatform {
    crosspad::SyncEventBus eventBus;
    MockClock clock;
    MockLedStrip ledStrip;
    crosspad::NullMidiOutput midiOutput;
    crosspad::PadManager padManager;
    crosspad::PadLedController padLedController;
    crosspad::PadAnimator padAnimator;
    crosspad::CrosspadStatus status;
    MemoryKVStore kvStore;

    ~CoreTestPlatform() {
        crosspad::crosspad_platform_reset();
    }

    bool init() {
        crosspad::CrosspadPlatformConfig config;
        config.eventBus         = &eventBus;
        config.clock            = &clock;
        config.ledStrip         = &ledStrip;
        config.midiOutput       = &midiOutput;
        config.padManager       = &padManager;
        config.padLedController = &padLedController;
        config.padAnimator      = &padAnimator;
        config.status           = &status;
        config.kvStore          = &kvStore;
        config.padCount         = 16;

        return crosspad::crosspad_platform_init(config);
    }
};

} // namespace test
