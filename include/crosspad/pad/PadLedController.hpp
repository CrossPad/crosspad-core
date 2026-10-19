#pragma once
#include <atomic>
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

    // Coalesced LED refresh: when enabled, updateLEDs()/updateSingleLED() only
    // write the pixel buffer and set a dirty flag; the actual ILedStrip
    // refresh() happens at most once per update() tick. On hardware refresh()
    // is an RMT transmission that interferes with the I2S audio DMA (random
    // output clicks), so audio platforms should enable this — but ONLY when
    // something ticks update() periodically, otherwise LEDs never repaint.
    // Off by default (refresh() fires immediately, the historical behavior).
    // Coalesced, notifyNotePlay() -- called from the audio render through
    // PadManager::handleSamplePlay() -- touches neither the pixels nor the
    // strip lock: it records the state and update() paints it.
    void setCoalescedRefresh(bool on) { coalesceRefresh_ = on; }
    bool getCoalescedRefresh() const { return coalesceRefresh_; }

    // MIDI event handling
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleNoteOff(uint8_t channel, uint8_t note);
    void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // Direct pad state control
    void notifyNoteOn(uint8_t padIdx, uint8_t velocity);
    void notifyNoteOff(uint8_t padIdx, uint8_t velocity = 0);
    void notifyAftertouch(uint8_t padIdx, uint8_t pressure);
    void notifyNotePlay(uint8_t padIdx, bool isPlaying);
    /// A Loop pad muted, or waiting for its bar: its playing colour's hue at
    /// half that colour's brightness, its brightest channel held between
    /// kDimmedMinPeak and kDimmedMaxPeak. Same threading as notifyNotePlay().
    void notifyNoteDimmed(uint8_t padIdx, bool dimmed);
    static constexpr uint8_t kDimmedMinPeak = 32;   ///< an eighth of full scale
    static constexpr uint8_t kDimmedMaxPeak = 64;   ///< a quarter of full scale

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
    // Drop a pad's custom-color override, returning it to default-managed state
    // (default/pressed/playing colors back to the globals, hasCustomColor=false).
    void clearPadColor(uint8_t padIdx);

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

    // Stop animation — real animations run in PadAnimator; this clears the
    // isAnimating_ flag/pad state and is invoked via the PadAnimator
    // completion callback (see CrosspadPlatform.cpp).
    void stopAnimation();

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

    // Global color defaults — match Pad's kit defaults (Pad.hpp: 0x20FF10 /
    // 0xFF1020) so pads look the same with and without an app palette loaded.
    RgbColor defaultColor_{0x20, 0xFF, 0x10};
    RgbColor pressedColor_{0xFF, 0x10, 0x20};
    RgbColor playingColor_{0x32, 0x00, 0x05};

    // Global state
    uint8_t brightness_ = 255;
    KeypadAnimationMode currentAnimMode_ = KeypadAnimationMode::NONE;
    KeypadFeedbackMode currentFeedbackMode_ = KeypadFeedbackMode::AFTERTOUCH;
    bool isAnimating_ = false;
    bool coalesceRefresh_ = false;
    bool ledsDirty_ = false; // guarded by the strip lock
    /// Coalesced: a pad's playing state changed, update() repaints. Set from
    /// the audio render, which may not wait for the strip lock.
    std::atomic<bool> repaint_{false};

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
    void refreshOrMarkDirty();
    void updateSingleLED(uint8_t padIdx);
    void updateDefaultColors();
    RgbColor calculatePadColor(uint8_t padIdx);
    void applyBrightness(RgbColor& color);
};

/// Platform-injected singleton accessor
PadLedController& getPadLedController();

} // namespace crosspad
