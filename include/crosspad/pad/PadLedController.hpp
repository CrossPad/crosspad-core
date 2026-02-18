#pragma once
#include <cstdint>
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/led/HslColor.hpp"
#include "crosspad/led/LedTypes.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

namespace crosspad {

/**
 * @brief Portable pad LED state and color controller.
 *
 * Manages per-pad color state, feedback modes, brightness, and LED updates.
 * Replaces the platform-specific KeypadRGBDriver. Uses ILedStrip for hardware
 * abstraction — the ILedStrip implementation handles pad→LED index remapping.
 */
class PadLedController {
public:
    PadLedController() = default;

    void init(ILedStrip& ledStrip, IEventBus& eventBus,
              CrosspadSettings* settings, uint8_t padCount = 16);
    void begin(); // subscribe EventBus, initial LED update

    void update();

    // MIDI event handling
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleNoteOff(uint8_t channel, uint8_t note);
    void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // Direct pad state control
    void notifyNoteOn(uint8_t padIdx, uint8_t velocity);
    void notifyNoteOff(uint8_t padIdx, uint8_t velocity = 0);
    void notifyNotePlay(uint8_t padIdx, bool isPlaying);

    // Color management
    void setPadColor(uint8_t padIdx, RgbColor color);
    void setPadPressedColor(uint8_t padIdx, RgbColor color);
    void setPadPlayingColor(uint8_t padIdx, RgbColor color);
    void setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    void setPadPressedColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    void setPadPlayingColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    void setDefaultColor(RgbColor color);
    void setPressedColor(RgbColor color);
    void setPlayingColor(RgbColor color);

    // Color getters
    RgbColor getPadColor(uint8_t padIdx);
    RgbColor getPressedColor(uint8_t padIdx);
    RgbColor getPlayingColor(uint8_t padIdx);

    // Mode management
    void setAnimationMode(KeypadAnimationMode mode);
    void setFeedbackMode(KeypadFeedbackMode mode);
    KeypadFeedbackMode getFeedbackMode();

    // Brightness
    void setBrightness(uint8_t brightness);
    uint8_t getBrightness();

    // Animations (legacy stubs — real animations are in PadAnimator)
    bool playAnimation(KeypadAnimationMode mode, KeypadAnimationParams params);
    bool playPadAnimation(uint8_t padIdx, KeypadAnimationParams params);
    void stopAnimation();
    bool isAnimationActive();

    // Animation flag (set by PadAnimator via callback)
    void setAnimating(bool v) { isAnimating_ = v; }
    bool getAnimating() const { return isAnimating_; }

    // Callbacks
    void setNoteOnCallback(KeypadNoteOnCallback callback);
    void setAftertouchCallback(KeypadAftertouchCallback callback);
    void setAnimationCompleteCallback(KeypadAnimationCompleteCallback callback);
    void setCustomColorCallback(KeypadCustomColorCallback callback);

    // Lookup
    int getPadFromMidiNote(uint8_t note);

    // Full LED refresh (public so glue code / PadAnimator can trigger restore)
    void updateLEDs();

private:
    ILedStrip* ledStrip_ = nullptr;
    IEventBus* eventBus_ = nullptr;
    CrosspadSettings* settings_ = nullptr;
    uint8_t padCount_ = 16;

    // Global color defaults
    RgbColor defaultColor_{0, 25, 0};
    RgbColor pressedColor_{125, 0, 0};
    RgbColor playingColor_{50, 0, 5};

    // Global state
    uint8_t brightness_ = 255;
    KeypadAnimationMode currentAnimMode_ = KeypadAnimationMode::NONE;
    KeypadFeedbackMode currentFeedbackMode_ = KeypadFeedbackMode::STANDARD;
    bool isAnimating_ = false;
    KeypadAnimationParams currentAnimParams_;

    // Per-pad state
    PadLedState padStates_[16];

    // Callbacks
    KeypadNoteOnCallback noteOnCallback_ = nullptr;
    KeypadAftertouchCallback aftertouchCallback_ = nullptr;
    KeypadAnimationCompleteCallback animationCompleteCallback_ = nullptr;
    KeypadCustomColorCallback customColorCallback_ = nullptr;

    // EventBus subscription
    SubscriptionHandle samplePlaybackSub_ = nullptr;

    // Internal
    void updateSingleLED(uint8_t padIdx);
    void updateDefaultColors();
    RgbColor calculatePadColor(uint8_t padIdx);
    void applyBrightness(RgbColor& color);
};

/// Platform-injected singleton accessor
PadLedController& getPadLedController();

} // namespace crosspad
