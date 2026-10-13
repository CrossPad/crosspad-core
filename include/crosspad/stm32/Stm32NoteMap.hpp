// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace crosspad {

// MIDI note the STM assigns to pad 0 at zero offset (BUTTONS_BASE_NOTE).
static constexpr uint8_t kStm32NoteBase = 36;

// Map a MIDI note received from the STM to a pad index, honoring the note
// offset the ESP pushed to REG_CTL_NOTE_OFFSET. STM emits 36 + pad + offset.
// Returns 0..15, or 0xFF if the note is outside the 16-pad range.
inline uint8_t stm32NoteToPad(uint8_t note, int noteOffset) {
    int base = (int)kStm32NoteBase + noteOffset;
    int idx = (int)note - base;
    if (idx < 0 || idx >= 16) return 0xFF;
    return (uint8_t)idx;
}

} // namespace crosspad
