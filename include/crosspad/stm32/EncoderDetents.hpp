#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief The encoder's steps as a hand means them: one per detent the knob
 *        goes into.
 *
 * The STM reports a detent index (0..15) and the sub-detent phase
 * (REG_IN_ENC_PHASE: [1:0] the count mod 4, [4] settled -- at rest in a
 * detent). Its index is (count + 1) >> 2, so it flips between counts 2 and 3;
 * the encoder on the board rests at count 2 mod 4, half a count below that
 * edge. A hair of overshoot past a detent flipped the index, the detent's
 * spring pulled the knob back and flipped it again: two steps from one click,
 * and a half turn to land on the item meant.
 *
 * So the index is not used as the step. The count is rebuilt from index and
 * phase, and a step is taken when the knob is three counts of four into the
 * next detent, or rests in it -- where the knob sits, not where the edge
 * happens to be. A wobble of one or two counts is no step either way.
 */
class EncoderDetents {
public:
    static constexpr uint8_t kSettled = 0x10;   ///< REG_IN_ENC_PHASE bit 4

    /// Feed the latest report; @return the steps since the last one, + for
    /// a rising count. Nothing counts until the knob is first reported at rest:
    /// before the STM's first report the input reads as count 0, and taking
    /// that for a position made a step out of the boot.
    int update(uint8_t top, uint8_t phase)
    {
        const int cnt = (4 * (top & 0x0F) + (((phase & 0x03) + 1) & 0x03) - 1) & 63;
        if (!primed_) {
            if (!(phase & kSettled)) return 0;
            primed_ = true;
            pos_ = cnt;
            return 0;
        }
        int d = cnt - pos_;
        if (d > 31)  d -= 64;
        if (d < -32) d += 64;
        int steps = 0;
        if (phase & kSettled) {
            /* In a detent: exactly the one it is in, three counts or more
             * counting as a detent, as they do on the way. */
            steps = d >= 0 ? (d + 1) / 4 : -((-d + 1) / 4);
            pos_ = cnt;
            return steps;
        }
        while (d >= 3)  { ++steps; d -= 4; pos_ = (pos_ + 4) & 63; }
        while (d <= -3) { --steps; d += 4; pos_ = (pos_ + 60) & 63; }
        return steps;
    }

private:
    bool primed_ = false;
    int  pos_ = 0;       ///< the detent last stepped into, as a count mod 64
};

} // namespace crosspad
