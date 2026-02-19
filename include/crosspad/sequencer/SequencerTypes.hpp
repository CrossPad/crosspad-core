#pragma once
#include <cstdint>

namespace crosspad {

/**
 * Sequencer operating mode.
 * Values match ML_MidiSeq's mode_e for zero-cost casting at the boundary.
 */
enum class SequencerMode : uint8_t {
    None        = 0,
    Playback    = 1,
    Record      = 2,
    Overdub     = 3,
    OverdubWait = 4
};

/**
 * Sequencer transport state.
 * Values match ML_MidiSeq's state_e for zero-cost casting at the boundary.
 */
enum class SequencerState : uint8_t {
    Idle        = 0,
    WaitForSync = 1,
    WaitNote    = 2,
    PreBeat     = 3,
    Running     = 4
};

/**
 * Note quantization grid.
 * Values match ML_MidiSeq quantization indices.
 */
enum class Quantization : uint8_t {
    Quarter      = 0,   // 1/4
    Eighth       = 1,   // 1/8
    Sixteenth    = 2,   // 1/16
    ThirtySecond = 3,   // 1/32
    Off          = 127
};

/// Sequencer constants
struct SequencerLimits {
    static constexpr uint8_t MAX_SEQ_LENGTH = 16;
    static constexpr uint8_t MAX_SEQ_INDEX  = 3;
    static constexpr float   MIN_BPM        = 20.0f;
    static constexpr float   MAX_BPM        = 300.0f;
    static constexpr Quantization DEFAULT_QUANTIZATION = Quantization::Sixteenth;
};

} // namespace crosspad
