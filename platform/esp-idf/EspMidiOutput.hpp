#pragma once
#include "crosspad/midi/IMidiOutput.hpp"
#include <functional>

namespace crosspad {

/**
 * @brief Callback-based MIDI output
 *
 * Platform code injects the actual send functions via setCallbacks().
 * This avoids tight coupling to application-layer functions.
 */
class EspMidiOutput : public IMidiOutput {
public:
    using NoteOnCallback = std::function<void(uint8_t note, uint8_t channel)>;
    using NoteOffCallback = std::function<void(uint8_t note, uint8_t channel)>;

    void setCallbacks(NoteOnCallback onNoteOn, NoteOffCallback onNoteOff) {
        noteOnCb_ = std::move(onNoteOn);
        noteOffCb_ = std::move(onNoteOff);
    }

    void sendNoteOn(uint8_t note, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t channel) override;

private:
    NoteOnCallback noteOnCb_;
    NoteOffCallback noteOffCb_;
};

} // namespace crosspad
