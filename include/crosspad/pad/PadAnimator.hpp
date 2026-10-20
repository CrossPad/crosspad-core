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
 * Uses ILedStrip and IClock only. The platform creates the instance and
 * wires the animation-state callback so PadLedController pauses its normal
 * LED updates while an animation runs.
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

    /// Wire the animator to the LED strip and clock. The platform calls it
    /// once at start-up.
    void init(ILedStrip& strip, IClock& clock);
    /// Render the running animation's next frame. PadManager::update() calls
    /// it, about every 10 ms.
    void update();

    /// Some animation runs -- not necessarily the caller's; see owns().
    bool isActive() const;

    /// True while the animation @p token started still runs: it has not
    /// finished, been stopped or been replaced by another start.
    bool owns(AnimToken token) const;

    /// Start a progress bar across the pads (as the firmware update shows),
    /// in @p color, at 0. It runs until stopped.
    AnimToken startProgress(RgbColor color = RgbColor(208, 240, 9));
    /// Move the progress bar to @p progress, 0.0 - 1.0 (clamped).
    void setProgress(float progress);

    /// Show a still frame, one colour per pad (index = pad), held until
    /// another animation or a stop: for a caller that draws its own animation
    /// -- the boot splash, pad for pad with the screen. Takes the pads from
    /// whatever ran.
    AnimToken showFrame(const RgbColor (&colors)[kPadCount]);
    /// Repaint the frame @p token showed. Returns false, showing nothing,
    /// once another animation has taken over.
    bool updateFrame(AnimToken token, const RgbColor (&colors)[kPadCount]);

    /// Every pad green, fading out over @p fadeMs; then the pads go dark and
    /// the animation ends.
    AnimToken playSuccess(uint32_t fadeMs = 800);
    /// Every pad flashes red three times (200 ms on, 200 ms off); then the
    /// animation ends. @p holdMs is ignored.
    AnimToken playError(uint32_t holdMs = 2000);

    /// The boot animation: a rainbow wipe across the pads, then a breathe,
    /// over @p durationMs.
    AnimToken playBootAnim(uint32_t durationMs = 600);

    /// Charge-while-off gauge, running until stopped: a battery gauge fills
    /// the 4x4 pads bottom-up, red to green by state of charge (@p soc,
    /// 0.0 - 1.0), a soft glow drifts over the lit pads, and the single
    /// frontier pad breathes. At soc == 1.0 (full) there is no frontier:
    /// all 16 pads green with the glow.
    AnimToken startCharging(float soc = 0.0f);
    /// Update the charging gauge's state of charge, 0.0 - 1.0 (clamped).
    void setChargePercent(float soc);
    /// Charging (true, the default at start) or plugged in and not charging
    /// (false: a still gauge).
    void setChargingActive(bool active);

    // Control
    /// Stops the animation @p token started. False, and nothing stopped, when
    /// it already ended or another animation took over.
    bool stop(AnimToken token);
    /// Stops whatever runs, whoever started it.
    void stopAll();

    using CompletionCallback = void (*)();
    /// One-shot: called once when the running animation ends by itself (not
    /// when it is stopped), outside the animator's lock, then cleared.
    void setCompletionCallback(CompletionCallback cb);

    /// Called with true when animation starts, false when it ends.
    /// The platform wires this to PadLedController::setAnimating / stopAnimation.
    using AnimationStateCallback = void (*)(bool active);
    /// Install the AnimationStateCallback.
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

/// The platform's PadAnimator.
PadAnimator& getPadAnimator();

} // namespace crosspad
