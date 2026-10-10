// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/midi/IBleMidi.hpp"

namespace crosspad {

/**
 * @brief No-op IBleMidi stub for platforms without BLE hardware.
 *
 * Used as the default in PlatformServices until a real BLE MIDI
 * implementation is registered via setBleMidi().
 */
class NullBleMidi : public IBleMidi {
public:
    bool begin(BleMidiMode) override { return false; }
    void end() override {}
    bool isConnected() const override { return false; }
    BleMidiMode getMode() const override { return BleMidiMode::Host; }

    void startScan(uint16_t) override {}
    void stopScan() override {}
    bool isScanning() const override { return false; }
    std::vector<BleMidiDevice> getScanResults() const override { return {}; }
    bool connectToDevice(const std::string&) override { return false; }

    void startAdvertising(const std::string&) override {}
    void stopAdvertising() override {}
    bool isAdvertising() const override { return false; }

    void disconnect() override {}
    BleMidiDevice getConnectedDevice() const override { return {}; }

    void sendNoteOn(uint8_t, uint8_t) override {}
    void sendNoteOff(uint8_t, uint8_t) override {}
};

} // namespace crosspad
