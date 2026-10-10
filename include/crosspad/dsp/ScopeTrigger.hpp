// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ScopeTrigger.hpp
 * @brief Edge trigger and window sizing for a waveform view, as on a scope.
 *
 * Drawing the newest N samples of a running engine every UI frame gives a wave
 * that slides sideways: the frame period and the note period are unrelated, so
 * each redraw starts at a different phase. A scope solves this with a trigger —
 * it starts the sweep at a rising edge — and so does this: scopeTrigger() picks
 * the *last* rising zero-crossing that still leaves a whole window after it, so
 * two snapshots taken an arbitrary time apart draw the same phase and the wave
 * stands still.
 *
 * scopeWindowSamples() sizes that window from the fundamental, so a low note
 * and a high note both show a few cycles rather than one wall of ink or a
 * single lazy curve.
 *
 * Pure functions over a caller-owned buffer — no state, no allocation. Feed
 * them a ScopeTap snapshot (audio/ScopeTap.hpp).
 */

#include <cstdint>

namespace crosspad {

/// Where the view should start inside a snapshot, and how much of it to draw.
struct ScopeWindow {
    uint32_t start = 0;
    uint32_t length = 0;
    bool triggered = false;  ///< false = no edge found; `start` is the tail
};

/// Cycles of the fundamental a window aims to show.
constexpr uint32_t kScopePeriods = 3;
/// Window floor, so a high note is not drawn from a handful of samples.
constexpr float kScopeMinWindowMs = 1.0f;
/// Window ceiling, so a low note (or no note at all) still redraws briskly.
constexpr float kScopeMaxWindowMs = 10.0f;
/**
 * @brief Trigger hysteresis, as a fraction of full scale.
 *
 * An edge counts only when the previous sample was below -kScopeHysteresis and
 * the current one is at or above +kScopeHysteresis. Without the band, dither
 * and the noise floor of a decayed note cross zero constantly and the trigger
 * latches onto whichever one happens to be last — the view jitters exactly when
 * there is nothing to see. 0.002 is ~ -54 dBFS: below any real waveform, above
 * the 16-bit floor of a quiet sample.
 */
constexpr float kScopeHysteresis = 0.002f;

/**
 * @brief Window length in samples for a fundamental of @p hz.
 *
 * kScopePeriods / hz, clamped to [kScopeMinWindowMs, kScopeMaxWindowMs].
 * @p hz <= 0 (no note yet) takes the maximum, which is also what any note
 * below 300 Hz gets. Returns 0 when @p sampleRate is 0.
 */
uint32_t scopeWindowSamples(float hz, uint32_t sampleRate);

/**
 * @brief Find the sweep start inside @p buf: the last rising zero-crossing
 *        that leaves @p length samples after it.
 *
 * Scans backwards from index `n - length`, so the view is as fresh as it can
 * be while still standing still. When nothing crosses — silence, a DC block, a
 * snapshot shorter than the window — the result is the tail of the buffer with
 * `triggered == false`, which draws the (flat) truth instead of nothing.
 *
 * @param n      samples available in @p buf.
 * @param length window length, typically from scopeWindowSamples().
 * @return start/length clamped to @p buf; `length` is shortened to @p n when
 *         the snapshot is shorter than the window asked for.
 */
ScopeWindow scopeTrigger(const float* buf, uint32_t n, uint32_t length);

} // namespace crosspad
