// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <mutex>
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/platform/IClock.hpp"

namespace crosspad {

/**
 * @brief Portable pad LED animator for progress bars, success/error/boot animations.
 *
 * Uses ILedStrip and IClock — no ESP-IDF dependencies.
 * The platform glue creates the instance and wires the animation-state callback
 * so PadLedController knows when to suppress normal LED updates.
 *
 * One animation runs at a time, and starting one replaces whatever ran: the
 * OTA bar or a loading animation always takes the pads. Every start returns
 * an AnimToken naming the animation it began, so a caller can ask whether its
 * animation still runs (owns()), repaint only its own frame (updateFrame())
 * and stop only what it started (stop(token)) -- never someone else's
 * animation that took over in between. stopAll() is for the system paths that
 * must clear the pads whoever drew on them (soft-off, sleep).
 *
 * Thread-safe: starts, stops and update() may come from different tasks. The
 * animation-state and completion callbacks run outside the internal lock, so
 * they may call back into the animator.
 */
class PadAnimator {
public:
    static constexpr uint8_t kPadCount = 16;

    /// Names one started animation. Never kNoAnim; a token is not reused.
    using AnimToken = uint32_t;
    static constexpr AnimToken kNoAnim = 0;

    PadAnimator() = default;

    void init(ILedStrip& strip, IClock& clock);
    void update(); // call every ~10 ms from animation task

    /// Some animation runs -- not necessarily the caller's; see owns().
    bool isActive() const;

    /// True while the animation @p token started still runs: it has not
    /// finished, been stopped or been replaced by another start.
    bool owns(AnimToken token) const;

    // Progress bar (OTA)
    AnimToken startProgress(RgbColor color = RgbColor(208, 240, 9));
    void setProgress(float progress); // 0.0 - 1.0

    // A still frame, one colour per pad (index = pad), held until another
    // animation or a stop: for a caller that draws its own animation -- the
    // boot splash, pad for pad with the screen. showFrame() starts it (taking
    // the pads from whatever ran); updateFrame() repaints it and returns false,
    // showing nothing, once another animation has taken over.
    AnimToken showFrame(const RgbColor (&colors)[kPadCount]);
    bool updateFrame(AnimToken token, const RgbColor (&colors)[kPadCount]);

    // End-state animations
    AnimToken playSuccess(uint32_t fadeMs = 800);
    AnimToken playError(uint32_t holdMs = 2000);

    // Boot animation
    AnimToken playBootAnim(uint32_t durationMs = 600);

    // Charge-while-off gauge. Persistent (runs until stop()): a battery gauge
    // fills the 4x4 pads bottom-up, color red->green by SoC, a soft concentric
    // glow drifts over the lit pads, and the single frontier pad breathes to 0.
    // At soc == 1.0 (full) there is no frontier — all 16 pads green + drift.
    AnimToken startCharging(float soc = 0.0f);
    void setChargePercent(float soc);   // 0.0 - 1.0, live updates while charging
    void setChargingActive(bool active); // false → static gauge (plugged, not charging)

    // Control
    /// Stops the animation @p token started. False, and nothing stopped, when
    /// it already ended or another animation took over.
    bool stop(AnimToken token);
    /// Stops whatever runs, whoever started it.
    void stopAll();

    using CompletionCallback = void (*)();
    void setCompletionCallback(CompletionCallback cb);

    /// Called with true when animation starts, false when it ends.
    /// Platform glue wires this to PadLedController::setAnimating / stopAnimation.
    using AnimationStateCallback = void (*)(bool active);
    void setAnimationStateCallback(AnimationStateCallback cb);

private:
    enum class AnimType { NONE, PROGRESS, SUCCESS, ERROR, BOOT, CHARGING, FRAME };

    mutable std::mutex mutex_;
    AnimType type_ = AnimType::NONE;
    AnimToken token_ = kNoAnim;      // the running animation's, kNoAnim when idle
    AnimToken lastToken_ = kNoAnim;
    ILedStrip* strip_ = nullptr;
    IClock* clock_ = nullptr;
    bool active_ = false;
    int64_t startTime_ = 0;
    uint32_t duration_ = 0;
    float currentProgress_ = 0.0f;
    float chargeSoc_ = 0.0f;          // 0..1 battery state-of-charge for CHARGING (caller pre-smooths)
    bool  chargingActive_ = true;     // false → static gauge (plugged but not charging)
    RgbColor animColor_;
    RgbColor frame_[kPadCount];
    CompletionCallback completionCb_ = nullptr;
    AnimationStateCallback animStateCb_ = nullptr;

    // Under mutex_: makes @p type the running animation, returns its token.
    AnimToken beginLocked(AnimType type);
    // Under mutex_: ends the running animation; true when one ran.
    bool endLocked();
    AnimToken begin(AnimType type);

    void setPixel(uint8_t padIdx, RgbColor color);
    void refresh();
    void finish();
    void notifyAnimState(bool active);

    void renderProgress();
    void renderSuccess();
    void renderError();
    void renderBoot();
    void renderCharging();
    void renderFrame();
};

/// Platform-injected singleton accessor
PadAnimator& getPadAnimator();

} // namespace crosspad
