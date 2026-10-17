#pragma once

#include <cstdint>

namespace crosspad {

/// The pads' 16 levels, as on the MPC: one pad -- the source -- on all
/// sixteen, at a loudness each (Velocity) or a semitone each (Pitch).
enum class PadLevels : uint8_t { Off = 0, Velocity = 1, Pitch = 2 };

/**
 * @brief What a pad press sounded as, beyond which pad it was.
 *
 * A performance mode turns the pad struck into something else: under 16
 * levels of pitch, pad 5 plays the source pad five semitones up. The loop
 * recorder keeps this with every hit, so a take plays back what was heard
 * whatever mode the pads are in by then -- the velocity a mode makes (Full
 * level, the levels' ramp) is resolved into the hit itself. Pad logic reads it
 * back for the recorder's own presses with PadManager::pressPerformance().
 */
struct PadPerformance {
    PadLevels levels = PadLevels::Off;
    uint8_t   source = 0;   ///< the pad that sounds, under 16 levels

    bool plain() const { return levels == PadLevels::Off; }

    /// One byte, for storage: the levels in the high nibble, the source in
    /// the low one. 0 is a plain hit.
    uint8_t pack() const
    {
        return static_cast<uint8_t>((static_cast<uint8_t>(levels) << 4) | (source & 0x0F));
    }
    /// The inverse; anything it does not know is a plain hit.
    static PadPerformance unpack(uint8_t b)
    {
        PadPerformance p;
        const uint8_t l = static_cast<uint8_t>(b >> 4);
        if (l > static_cast<uint8_t>(PadLevels::Pitch)) return p;
        p.levels = static_cast<PadLevels>(l);
        p.source = static_cast<uint8_t>(b & 0x0F);
        return p;
    }
};

/**
 * @brief The pads' performance mode, for an app that does not own it.
 *
 * The Sampler's pad logic has 16 levels; the Sequencer turns it on to record
 * a part in it. Null in PlatformServices where nothing offers one.
 */
class IPadPerformance {
public:
    virtual ~IPadPerformance() = default;
    virtual PadLevels levels() const = 0;
    virtual void setLevels(PadLevels mode) = 0;
    /// The pad 16 levels plays: the one hit last before it went on.
    virtual uint8_t levelsSource() const = 0;
};

} // namespace crosspad
