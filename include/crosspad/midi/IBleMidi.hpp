#pragma once

/**
 * @file IBleMidi.hpp
 * @brief Portable BLE MIDI interface — mirrors PcMidi/USB MIDI pattern.
 *
 * Implements IMidiOutput for outbound MIDI. Provides input callbacks
 * (setHandleNoteOn etc.) for inbound MIDI. Platform implementations
 * (SimpleBLE on PC, NimBLE on ESP32) inherit this and provide the
 * actual BLE transport.
 *
 * Three roles:
 *  - IMidiOutput: outbound MIDI (pad presses → BLE)
 *  - Input callbacks: inbound MIDI (BLE → PadManager/EventBus)
 *  - BLE lifecycle: scan, connect, advertise, disconnect
 */

#include "crosspad/midi/IMidiOutput.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace crosspad {

/// BLE MIDI operating mode
enum class BleMidiMode : uint8_t {
    Host   = 0,   ///< Central — scans for and connects to BLE MIDI peripherals
    Server = 1,   ///< Peripheral — advertises and accepts incoming connections
};

/// Discovered or connected BLE MIDI device descriptor
struct BleMidiDevice {
    std::string name;
    std::string address;          ///< MAC or platform identifier
    int8_t      rssi      = 0;   ///< Signal strength (scan results only)
    bool        connected = false;
};

class IBleMidi : public IMidiOutput {
public:
    // ── Callback types (same signatures as PcMidi) ──────────────────
    using NoteOnCallback        = std::function<void(uint8_t channel, uint8_t note, uint8_t velocity)>;
    using NoteOffCallback       = std::function<void(uint8_t channel, uint8_t note, uint8_t velocity)>;
    using ControlChangeCallback = std::function<void(uint8_t channel, uint8_t cc, uint8_t value)>;
    using ConnectionCallback    = std::function<void(bool connected, const BleMidiDevice& device)>;
    using ScanResultCallback    = std::function<void(const std::vector<BleMidiDevice>& devices)>;

    ~IBleMidi() override = default;

    // ── Lifecycle ────────────────────────────────────────────────────
    virtual bool begin(BleMidiMode mode) = 0;
    virtual void end() = 0;
    virtual bool isConnected() const = 0;
    virtual BleMidiMode getMode() const = 0;

    // ── Host mode (Central) ─────────────────────────────────────────
    virtual void startScan(uint16_t durationMs = 5000) = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() const = 0;
    virtual std::vector<BleMidiDevice> getScanResults() const = 0;
    virtual bool connectToDevice(const std::string& address) = 0;

    // ── Server mode (Peripheral) ────────────────────────────────────
    virtual void startAdvertising(const std::string& deviceName = "CrossPad") = 0;
    virtual void stopAdvertising() = 0;
    virtual bool isAdvertising() const = 0;

    // ── Common ──────────────────────────────────────────────────────
    virtual void disconnect() = 0;
    virtual BleMidiDevice getConnectedDevice() const = 0;

    // ── Input callbacks (mirrors PcMidi setHandle* pattern) ─────────
    void setHandleNoteOn(NoteOnCallback cb)               { noteOnCb_ = std::move(cb); }
    void setHandleNoteOff(NoteOffCallback cb)             { noteOffCb_ = std::move(cb); }
    void setHandleControlChange(ControlChangeCallback cb) { ccCb_ = std::move(cb); }
    void setOnConnectionChanged(ConnectionCallback cb)    { connCb_ = std::move(cb); }
    void setOnScanResults(ScanResultCallback cb)          { scanCb_ = std::move(cb); }

    // ── Note offsets ────────────────────────────────────────────────
    /// Inbound transpose, applied transparently in dispatch.
    void   setNoteOffset(int8_t offset) { noteOffset_ = offset; }
    int8_t getNoteOffset() const        { return noteOffset_; }

    /// Outbound transpose, applied by the implementation on the way out (see
    /// BleMidiOffsets.hpp for the picklist the UI offers). 0 sends exactly what
    /// the router handed over — the same note USB MIDI is sending.
    void   setTxNoteOffset(int8_t offset) { txNoteOffset_ = offset; }
    int8_t getTxNoteOffset() const        { return txNoteOffset_; }

protected:
    /// Subclasses call these from their BLE notification handlers.
    /// Note offset is applied here so platform code doesn't need to care.
    void dispatchNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
        if (noteOnCb_) {
            int shifted = note + noteOffset_;
            if (shifted >= 0 && shifted <= 127)
                noteOnCb_(ch, static_cast<uint8_t>(shifted), vel);
        }
    }
    void dispatchNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
        if (noteOffCb_) {
            int shifted = note + noteOffset_;
            if (shifted >= 0 && shifted <= 127)
                noteOffCb_(ch, static_cast<uint8_t>(shifted), vel);
        }
    }
    void dispatchCC(uint8_t ch, uint8_t cc, uint8_t val) {
        if (ccCb_) ccCb_(ch, cc, val);
    }
    void dispatchConnectionChanged(bool connected, const BleMidiDevice& dev) {
        if (connCb_) connCb_(connected, dev);
    }
    void dispatchScanResults(const std::vector<BleMidiDevice>& devices) {
        if (scanCb_) scanCb_(devices);
    }

    /// Shift a note for transmission. False when the result leaves 0..127, in
    /// which case the message must be dropped rather than wrapped: a wrapped
    /// note is a wrong note, and a silent one is at least honest.
    bool applyTxOffset(uint8_t note, uint8_t& out) const {
        int shifted = (int)note + txNoteOffset_;
        if (shifted < 0 || shifted > 127) return false;
        out = (uint8_t)shifted;
        return true;
    }

    int8_t noteOffset_ = 0;
    int8_t txNoteOffset_ = 0;

private:
    NoteOnCallback        noteOnCb_;
    NoteOffCallback       noteOffCb_;
    ControlChangeCallback ccCb_;
    ConnectionCallback    connCb_;
    ScanResultCallback    scanCb_;
};

/// Platform-injected singleton accessor (defined in PlatformServices.cpp)
IBleMidi& getBleMidi();

} // namespace crosspad
