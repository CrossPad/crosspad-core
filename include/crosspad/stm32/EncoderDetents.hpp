#pragma once

#include <cstdint>

namespace crosspad {

/**
 * @brief The encoder's steps as a hand means them: one per detent the knob
 *        goes into, read from its A/B lines.
 *
 * The STM reports, with every change, its LPTIM count and the live A/B pins
 * (REG_IN_ENC_PHASE: [1:0] the count mod 4, [2] A, [3] B, [4] A && B). The
 * count cannot be trusted: on this board the LPTIM gains counts -- one click
 * measured 7 of them, four at a single edge -- and where the knob rests
 * drifts from 1 mod 4 through 2, 3 and 0 within a minute of turning, so a
 * step taken from the count was two steps for one click. The pins never
 * lie: each click is the same Gray cycle, (1,1) at rest.
 *
 * So the pins are decoded, one quadrature state at a time, and a step is
 * taken three states into the next detent -- before the knob lands, as a
 * hand expects -- with the rest state resetting the count: whatever was
 * counted on the way, the knob is in a detent there. Going back two states
 * is what undoes a step; a wobble of one is nothing.
 *
 * Feed it every report as it arrives, not the latest one now and then: a
 * reader polling at the display's rate misses states of a fast turn.
 */
class EncoderDetents {
public:
    /// Feed a report's phase byte; @return the steps it completes, + where
    /// the STM's count rises.
    int update(uint8_t phase)
    {
        /* Position in the detent's cycle, the way the count rises: (A,B) =
         * (1,1) rest, (0,1), (0,0), (1,0). Indexed by A << 1 | B. */
        static constexpr int8_t kAt[4] = {2, 1, 3, 0};
        const int at = kAt[(((phase >> 2) & 1) << 1) | ((phase >> 3) & 1)];
        if (!primed_) {
            primed_ = true;
            at_ = at;
            return 0;
        }
        const int d = (at - at_) & 3;
        at_ = at;
        if (d == 0) return 0;
        /* Two states on at once: a report was missed, and the knob went on
         * the way it was going. */
        const int move = d == 1 ? 1 : d == 3 ? -1 : 2 * dir_;
        if (d != 2) dir_ = move;
        sub_ += move;
        int steps = 0;
        if (sub_ >= 3)       { steps = 1;  sub_ -= 4; }
        else if (sub_ <= -3) { steps = -1; sub_ += 4; }
        if (at == 0) sub_ = 0;
        return steps;
    }

private:
    bool primed_ = false;
    int  at_ = 0;    ///< the last state, 0 = rest
    int  sub_ = 0;   ///< states moved since the last step or rest
    int  dir_ = 0;   ///< the last direction seen, for a skipped state
};

} // namespace crosspad
