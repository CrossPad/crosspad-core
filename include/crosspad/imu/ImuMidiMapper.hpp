#pragma once

/**
 * @file ImuMidiMapper.hpp
 * @brief Board motion as MIDI: tilt and rotation driving CCs or pitch bend.
 *
 * Portable on purpose — the mapping is arithmetic over numbers the IMU already
 * produces, so ESP-IDF, Arduino and the PC simulator share it and a change to
 * the feel of the controller is one change, not three.
 *
 * The platform feeds update() at whatever rate its IMU task runs (100 Hz on
 * ESP-IDF); this class decides what actually goes on the wire. Two rules keep it
 * off the link when nobody is moving the board:
 *
 *   - a minimum interval between messages (50 Hz by default), because USB MIDI
 *     on this device has already been starved once by bursts, and
 *   - a value that quantises to what was last sent is not sent again, so a board
 *     lying on a desk produces no traffic at all.
 *
 * What it deliberately does not do: turn a shake into a note. Striking is what
 * the pads are for.
 */

#include <cstdint>
#include <cstddef>

namespace crosspad {

class IMidiOutput;

/// What drives a slot. Angles come from the fused filter, rates from the gyro.
enum class ImuMidiSource : uint8_t {
    Pitch = 0,   ///< fused pitch, degrees
    Roll,        ///< fused roll, degrees
    GyroX,       ///< angular rate, degrees/s
    GyroY,
    GyroZ,
    Count
};

/// Where a slot sends. CC is 7-bit; PitchBend is the 14-bit channel message.
enum class ImuMidiTarget : uint8_t {
    ControlChange = 0,
    PitchBend,
    Count
};

/**
 * @brief One mapping: a source, a destination, and the span that fills it.
 *
 * @c inputSpan is the half-range that reaches full scale: 45 means -45..+45
 * degrees (or dps) covers the whole output. Angles are centred by nature, so a
 * CC sits at 64 when the board is level and a pitch bend sits at zero.
 */
struct ImuMidiSlot {
    bool           enabled  = false;
    ImuMidiSource  source   = ImuMidiSource::Pitch;
    ImuMidiTarget  target   = ImuMidiTarget::ControlChange;
    uint8_t        cc       = 1;      ///< controller number when target is CC
    uint8_t        channel  = 0;      ///< 0-15
    bool           invert   = false;
    float          inputSpan = 45.0f; ///< degrees for angles, deg/s for gyro
};

/// Four is enough for pitch + roll + two gyro axes, which is the useful shape.
static constexpr size_t kImuMidiSlots = 4;

class ImuMidiMapper {
public:
    /// Minimum gap between messages for a single slot.
    static constexpr uint32_t kDefaultMinIntervalMs = 20;   // 50 Hz

    void setOutput(IMidiOutput* out) { out_ = out; }

    /// Master switch. Turning it off also forgets what was last sent, so the
    /// next enable starts by transmitting the current attitude rather than
    /// staying silent because it happens to match a stale value.
    void setEnabled(bool on);
    bool enabled() const { return enabled_; }

    ImuMidiSlot&       slot(size_t i)       { return slots_[i < kImuMidiSlots ? i : 0]; }
    const ImuMidiSlot& slot(size_t i) const { return slots_[i < kImuMidiSlots ? i : 0]; }

    void setMinIntervalMs(uint32_t ms) { minIntervalMs_ = ms; }

    /**
     * @brief Feed one IMU sample. Sends whatever changed, nothing else.
     *
     * @param nowMs monotonic milliseconds; the only clock this class needs.
     * @return how many MIDI messages were sent, for counters and tests.
     */
    unsigned update(float pitchDeg, float rollDeg,
                    float gyroXDps, float gyroYDps, float gyroZDps,
                    uint32_t nowMs);

    /// Messages sent since construction — what a status verb reports.
    uint32_t sent() const { return sent_; }

private:
    float sourceValue(ImuMidiSource s, float pitch, float roll,
                      float gx, float gy, float gz) const;

    IMidiOutput* out_ = nullptr;
    bool         enabled_ = false;
    uint32_t     minIntervalMs_ = kDefaultMinIntervalMs;
    ImuMidiSlot  slots_[kImuMidiSlots]{};
    int32_t      lastSent_[kImuMidiSlots]{};
    bool         primed_[kImuMidiSlots]{};
    uint32_t     lastSendMs_[kImuMidiSlots]{};
    uint32_t     sent_ = 0;
};

} // namespace crosspad
