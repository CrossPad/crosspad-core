#include "crosspad/imu/ImuMidiMapper.hpp"
#include "crosspad/midi/IMidiOutput.hpp"

namespace crosspad {

void ImuMidiMapper::setEnabled(bool on)
{
    enabled_ = on;
    /* Forget the last value on either edge. Without this, enabling while the
     * board happens to sit where it sat when it was disabled would send nothing,
     * and the host would hold a stale controller value until someone moved. */
    for (size_t i = 0; i < kImuMidiSlots; ++i) primed_[i] = false;
}

float ImuMidiMapper::sourceValue(ImuMidiSource s, float pitch, float roll,
                                 float gx, float gy, float gz) const
{
    switch (s) {
        case ImuMidiSource::Pitch: return pitch;
        case ImuMidiSource::Roll:  return roll;
        case ImuMidiSource::GyroX: return gx;
        case ImuMidiSource::GyroY: return gy;
        case ImuMidiSource::GyroZ: return gz;
        default:                   return 0.0f;
    }
}

unsigned ImuMidiMapper::update(float pitchDeg, float rollDeg,
                               float gyroXDps, float gyroYDps, float gyroZDps,
                               uint32_t nowMs)
{
    if (!enabled_ || out_ == nullptr) return 0;

    unsigned messages = 0;
    for (size_t i = 0; i < kImuMidiSlots; ++i) {
        const ImuMidiSlot &s = slots_[i];
        if (!s.enabled) continue;
        if (s.inputSpan <= 0.0f) continue;   // a zero span has no mapping to make

        /* Rate limit per slot, not globally: two slots may legitimately move at
         * once, and throttling them together would make one starve the other. */
        if (primed_[i] && (uint32_t)(nowMs - lastSendMs_[i]) < minIntervalMs_) continue;

        float norm = sourceValue(s.source, pitchDeg, rollDeg,
                                 gyroXDps, gyroYDps, gyroZDps) / s.inputSpan;
        if (norm > 1.0f) norm = 1.0f;
        if (norm < -1.0f) norm = -1.0f;
        if (s.invert) norm = -norm;

        int32_t value;
        if (s.target == ImuMidiTarget::PitchBend) {
            /* -8192..8191: the negative side has one more step than the
             * positive, so scale by the smaller half and keep zero at zero. */
            value = (int32_t)(norm * 8191.0f);
            if (value < -8192) value = -8192;
            if (value > 8191) value = 8191;
        } else {
            // Level board sits at 64, the centre a host expects of a bipolar CC.
            value = (int32_t)(norm * 63.0f) + 64;
            if (value < 0) value = 0;
            if (value > 127) value = 127;
        }

        // Nothing changed at the resolution the wire carries: say nothing.
        if (primed_[i] && value == lastSent_[i]) continue;

        if (s.target == ImuMidiTarget::PitchBend) {
            out_->sendPitchBend((int16_t)value, s.channel);
        } else {
            out_->sendControlChange(s.cc, (uint8_t)value, s.channel);
        }
        lastSent_[i]   = value;
        primed_[i]     = true;
        lastSendMs_[i] = nowMs;
        ++messages;
        ++sent_;
    }
    return messages;
}

} // namespace crosspad
