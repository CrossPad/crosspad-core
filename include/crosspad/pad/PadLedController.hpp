// SPDX-License-Identifier: GPL-3.0-or-later
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
 * Uses ILedStrip for hardware abstraction -- the ILedStrip implementation
 * handles pad-to-LED index remapping. Apps normally reach it through
 * PadManager, which forwards the colour, brightness and feedback calls.
 */
class PadLedController {
public:
    PadLedController() = default;

    /// Wire the controller to the LED strip, event bus and settings. The
    /// platform calls it once at start-up.
    void init(ILedStrip& ledStrip, IEventBus& eventBus,
              CrosspadSettings* settings, uint8_t padCount = 16);
    /// Start the strip, follow EventType::SamplePlayback for the playing
    /// state, and paint the pads. The platform calls it once, after init().
    void begin();

    /// Paint what changed since the last tick when coalesced refresh is on.
    /// PadManager::update() calls it.
    void update();

    /// Coalesced LED refresh: when on, LED changes only update the pixel
    /// buffer, and the strip is refreshed at most once per update() tick.
    /// Platforms that play audio turn it on, because on hardware a strip
    /// refresh disturbs the audio output; it needs update() ticked
    /// periodically, or the LEDs never repaint. Off by default: the strip is
    /// refreshed at once.
    ///
    /// When on, notifyNotePlay() -- called from the audio render through
    /// PadManager::handleSamplePlay() -- touches neither the pixels nor the
    /// strip lock: it records the state and update() paints it.
    void setCoalescedRefresh(bool on) { coalesceRefresh_ = on; }
    /// Whether coalesced refresh is on.
    bool getCoalescedRefresh() const { return coalesceRefresh_; }

    // ── MIDI input (by note) ────────────────────────────────────────────
    /// Light the pad whose LED state carries @p note.
    void handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    /// Release the pad whose LED state carries @p note.
    void handleNoteOff(uint8_t channel, uint8_t note);
    /// Pressure for the pad whose LED state carries @p note.
    void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // ── Pad state (by pad index) ────────────────────────────────────────
    /// The pad is held, at @p velocity: repaint it.
    void notifyNoteOn(uint8_t padIdx, uint8_t velocity);
    /// The pad is released: repaint it.
    void notifyNoteOff(uint8_t padIdx, uint8_t velocity = 0);
    /// Live pressure (0-127) on the pad; in KeypadFeedbackMode::AFTERTOUCH
    /// a held pad's brightness follows it.
    void notifyAftertouch(uint8_t padIdx, uint8_t pressure);
    /// A sample started or stopped sounding on the pad.
    void notifyNotePlay(uint8_t padIdx, bool isPlaying);

    // ── Colours ─────────────────────────────────────────────────────────
    /// Give one pad a resting colour of its own; the global colours no
    /// longer change that pad until clearPadColor().
    void setPadColor(uint8_t padIdx, RgbColor color);
    /// The colour of one pad while it is held.
    void setPadPressedColor(uint8_t padIdx, RgbColor color);
    /// The colour of one pad while its sample sounds.
    void setPadPlayingColor(uint8_t padIdx, RgbColor color);
    /// setPadColor() from 8-bit red, green and blue.
    void setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    /// setPadPressedColor() from 8-bit red, green and blue.
    void setPadPressedColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    /// setPadPlayingColor() from 8-bit red, green and blue.
    void setPadPlayingColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    /// Resting colour of every pad without a colour of its own.
    void setDefaultColor(RgbColor color);
    /// Held colour of every pad without a colour of its own.
    void setPressedColor(RgbColor color);
    /// Playing colour of every pad without a colour of its own.
    void setPlayingColor(RgbColor color);
    /// Drop a pad's own colour: its resting, pressed and playing colours go
    /// back to the global ones, and the global setters change it again.
    void clearPadColor(uint8_t padIdx);

    /// The pad's resting colour.
    RgbColor getPadColor(uint8_t padIdx);
    /// The pad's colour while held.
    RgbColor getPressedColor(uint8_t padIdx);
    /// The pad's colour while its sample sounds.
    RgbColor getPlayingColor(uint8_t padIdx);

    // ── Modes ───────────────────────────────────────────────────────────
    /// Record the animation mode; KeypadAnimationMode::NONE also calls
    /// stopAnimation(). Animations themselves run in PadAnimator.
    void setAnimationMode(KeypadAnimationMode mode);
    /// How the pads light when pressed (see KeypadFeedbackMode).
    void setFeedbackMode(KeypadFeedbackMode mode);
    /// The current feedback mode.
    KeypadFeedbackMode getFeedbackMode();

    /// LED brightness, 0-255. The lower of this and the LED brightness in
    /// Settings applies.
    void setBrightness(uint8_t brightness);
    /// The brightness set with setBrightness().
    uint8_t getBrightness();

    /// Clear the animating state and repaint the pads' own colours. The
    /// platform calls it when a PadAnimator animation ends.
    void stopAnimation();

    /// Set while a PadAnimator animation owns the pads; normal LED updates
    /// pause meanwhile. The platform wires it to the animator's state
    /// callback.
    void setAnimating(bool v) { isAnimating_ = v; }
    /// Whether an animation owns the pads.
    bool getAnimating() const { return isAnimating_; }

    // ── Callbacks ───────────────────────────────────────────────────────
    /// Called on every pad press and release (notifyNoteOn()/notifyNoteOff()).
    void setNoteOnCallback(KeypadNoteOnCallback callback);
    /// Called on every pressure change.
    void setAftertouchCallback(KeypadAftertouchCallback callback);
    /// Set the per-pad animation-complete callback. Animations run in
    /// PadAnimator; for the end of one, use PadAnimator::setCompletionCallback().
    void setAnimationCompleteCallback(KeypadAnimationCompleteCallback callback);
    /// The function that colours every pad in KeypadFeedbackMode::APP_DEFINED.
    void setCustomColorCallback(KeypadCustomColorCallback callback);

    /// The pad whose LED state carries @p note, or -1.
    int getPadFromMidiNote(uint8_t note);

    /// Repaint every pad now. Public so the platform and PadAnimator can
    /// restore the pads after an animation.
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

/// The platform's PadLedController.
PadLedController& getPadLedController();

} // namespace crosspad
