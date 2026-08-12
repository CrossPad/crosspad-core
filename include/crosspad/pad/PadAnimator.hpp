#pragma once
#include <cstdint>
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
 */
class PadAnimator {
public:
    static constexpr uint8_t kPadCount = 16;

    PadAnimator() = default;

    void init(ILedStrip& strip, IClock& clock);
    void update(); // call every ~10 ms from animation task

    bool isActive() const { return active_; }

    // Progress bar (OTA)
    void startProgress(RgbColor color = RgbColor(208, 240, 9));
    void setProgress(float progress); // 0.0 - 1.0

    // End-state animations
    void playSuccess(uint32_t fadeMs = 800);
    void playError(uint32_t holdMs = 2000);

    // Boot animation
    void playBootAnim(uint32_t durationMs = 600);

    // Charge-while-off gauge. Persistent (runs until stop()): a battery gauge
    // fills the 4x4 pads bottom-up, color red->green by SoC, a soft concentric
    // glow drifts over the lit pads, and the single frontier pad breathes to 0.
    // At soc == 1.0 (full) there is no frontier — all 16 pads green + drift.
    void startCharging(float soc = 0.0f);
    void setChargePercent(float soc);   // 0.0 - 1.0, live updates while charging
    void setChargingActive(bool active); // false → static gauge (plugged, not charging)

    // Control
    void stop();

    using CompletionCallback = void (*)();
    void setCompletionCallback(CompletionCallback cb);

    /// Called with true when animation starts, false when it ends.
    /// Platform glue wires this to PadLedController::setAnimating / stopAnimation.
    using AnimationStateCallback = void (*)(bool active);
    void setAnimationStateCallback(AnimationStateCallback cb);

private:
    enum class AnimType { NONE, PROGRESS, SUCCESS, ERROR, BOOT, CHARGING };

    AnimType type_ = AnimType::NONE;
    ILedStrip* strip_ = nullptr;
    IClock* clock_ = nullptr;
    bool active_ = false;
    int64_t startTime_ = 0;
    uint32_t duration_ = 0;
    float currentProgress_ = 0.0f;
    float chargeSoc_ = 0.0f;          // 0..1 battery state-of-charge for CHARGING (caller pre-smooths)
    bool  chargingActive_ = true;     // false → static gauge (plugged but not charging)
    RgbColor animColor_;
    CompletionCallback completionCb_ = nullptr;
    AnimationStateCallback animStateCb_ = nullptr;

    void setPixel(uint8_t padIdx, RgbColor color);
    void refresh();
    void finish();
    void notifyAnimState(bool active);

    void renderProgress();
    void renderSuccess();
    void renderError();
    void renderBoot();
    void renderCharging();
};

/// Platform-injected singleton accessor
PadAnimator& getPadAnimator();

} // namespace crosspad
