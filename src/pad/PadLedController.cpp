#include "crosspad/pad/PadLedController.hpp"
#include <cstdio>
#include <algorithm>

namespace crosspad {

void PadLedController::init(ILedStrip& ledStrip, IEventBus& eventBus,
                             CrosspadSettings* settings, uint8_t padCount) {
    ledStrip_ = &ledStrip;
    eventBus_ = &eventBus;
    settings_ = settings;
    padCount_ = padCount;
}

void PadLedController::begin() {
    // Subscribe to EventBus for sampler feedback
    if (eventBus_) {
        samplePlaybackSub_ = eventBus_->subscribe(EventType::SamplePlayback,
            [](const void* event_data, EventType, void* ctx) {
                auto* data = static_cast<const SamplePlaybackData*>(event_data);
                auto* ctrl = static_cast<PadLedController*>(ctx);
                ctrl->notifyNotePlay(data->note, data->isOn);
            }, this);
    }

    // Set initial colors and show
    updateLEDs();
}

void PadLedController::update() {
    if (isAnimating_) return;
}

// ── LED update methods ──────────────────────────────────────────────────

void PadLedController::updateLEDs() {
    if (isAnimating_ || !ledStrip_) return;
    ledStrip_->lock();
    if (isAnimating_) { ledStrip_->unlock(); return; } // double-check after lock
    for (int i = 0; i < padCount_; i++) {
        RgbColor color = calculatePadColor(i);
        ledStrip_->setPixel(i, color);
    }
    ledStrip_->refresh();
    ledStrip_->unlock();
}

void PadLedController::updateSingleLED(uint8_t padIdx) {
    if (padIdx >= padCount_ || !ledStrip_) return;
    if (isAnimating_) return;
    ledStrip_->lock();
    if (isAnimating_) { ledStrip_->unlock(); return; }
    RgbColor color = calculatePadColor(padIdx);
    ledStrip_->setPixel(padIdx, color);
    ledStrip_->refresh();
    ledStrip_->unlock();
}

// ── MIDI event handling ─────────────────────────────────────────────────

void PadLedController::handleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    int padIdx = getPadFromMidiNote(note);
    if (padIdx >= 0) {
        notifyNoteOn(padIdx, velocity);
    }
}

void PadLedController::handleNoteOff(uint8_t channel, uint8_t note) {
    int padIdx = getPadFromMidiNote(note);
    if (padIdx >= 0) {
        notifyNoteOff(padIdx);
    }
}

void PadLedController::handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    int padIdx = getPadFromMidiNote(note);
    if (padIdx >= 0) {
        padStates_[padIdx].pressure = pressure;

        if (aftertouchCallback_) {
            aftertouchCallback_(padIdx, pressure);
        }

        if (currentFeedbackMode_ == KeypadFeedbackMode::AFTERTOUCH && padStates_[padIdx].isPressed) {
            updateLEDs();
        }
    }
}

// ── Direct pad state control ────────────────────────────────────────────

void PadLedController::notifyNoteOn(uint8_t padIdx, uint8_t velocity) {
    if (padIdx >= 16) return;

    padStates_[padIdx].isPressed = true;
    padStates_[padIdx].velocity = velocity;

    if (noteOnCallback_) {
        noteOnCallback_(padIdx, true, velocity);
    }

    updateSingleLED(padIdx);
}

void PadLedController::notifyNoteOff(uint8_t padIdx, uint8_t velocity) {
    if (padIdx >= 16) return;

    padStates_[padIdx].isPressed = false;

    if (noteOnCallback_) {
        noteOnCallback_(padIdx, false, velocity);
    }

    updateSingleLED(padIdx);
}

void PadLedController::notifyNotePlay(uint8_t padIdx, bool isPlaying) {
    if (padIdx >= 16) return;

    padStates_[padIdx].isPlaying = isPlaying;

    if (!padStates_[padIdx].isPressed) {
        updateLEDs();
    }
}

// ── Color management ────────────────────────────────────────────────────

void PadLedController::setPadColor(uint8_t padIdx, RgbColor color) {
    if (padIdx < 16) {
        padStates_[padIdx].defaultColor = color;
        padStates_[padIdx].hasCustomColor = true;

        if (!padStates_[padIdx].isPressed && !padStates_[padIdx].isPlaying) {
            updateLEDs();
        }
    }
}

void PadLedController::setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b) {
    setPadColor(padIdx, RgbColor(r, g, b));
}

void PadLedController::setPadPressedColor(uint8_t padIdx, RgbColor color) {
    if (padIdx < 16) {
        padStates_[padIdx].pressedColor = color;
        if (padStates_[padIdx].isPressed) {
            updateLEDs();
        }
    }
}

void PadLedController::setPadPressedColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b) {
    setPadPressedColor(padIdx, RgbColor(r, g, b));
}

void PadLedController::setPadPlayingColor(uint8_t padIdx, RgbColor color) {
    if (padIdx < 16) {
        padStates_[padIdx].playingColor = color;
        if (padStates_[padIdx].isPlaying && !padStates_[padIdx].isPressed) {
            updateLEDs();
        }
    }
}

void PadLedController::setPadPlayingColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b) {
    setPadPlayingColor(padIdx, RgbColor(r, g, b));
}

void PadLedController::setDefaultColor(RgbColor color) {
    defaultColor_ = color;
    updateDefaultColors();
}

void PadLedController::setPressedColor(RgbColor color) {
    pressedColor_ = color;
    updateDefaultColors();
}

void PadLedController::setPlayingColor(RgbColor color) {
    playingColor_ = color;
    updateDefaultColors();
}

RgbColor PadLedController::getPadColor(uint8_t padIdx) {
    if (padIdx < 16) return padStates_[padIdx].defaultColor;
    return RgbColor(0, 0, 0);
}

RgbColor PadLedController::getPressedColor(uint8_t padIdx) {
    if (padIdx < 16) return padStates_[padIdx].pressedColor;
    return pressedColor_;
}

RgbColor PadLedController::getPlayingColor(uint8_t padIdx) {
    if (padIdx < 16) return padStates_[padIdx].playingColor;
    return playingColor_;
}

void PadLedController::updateDefaultColors() {
    for (int i = 0; i < 16; i++) {
        if (!padStates_[i].hasCustomColor) {
            padStates_[i].defaultColor = defaultColor_;
            padStates_[i].pressedColor = pressedColor_;
            padStates_[i].playingColor = playingColor_;
        }
    }
    updateLEDs();
}

// ── Mode management ─────────────────────────────────────────────────────

void PadLedController::setAnimationMode(KeypadAnimationMode mode) {
    currentAnimMode_ = mode;
    if (mode == KeypadAnimationMode::NONE) {
        stopAnimation();
    }
}

void PadLedController::setFeedbackMode(KeypadFeedbackMode mode) {
    currentFeedbackMode_ = mode;

    if (mode == KeypadFeedbackMode::SPECTRUM) {
        for (int i = 0; i < padCount_; i++) {
            float hue = i / (float)padCount_;
            HslColor hslColor(hue, 1.0f, 0.5f);
            RgbColor color = RgbColor(hslColor);
            padStates_[i].defaultColor = color;
            padStates_[i].pressedColor = RgbColor(255 - color.R, 255 - color.G, 255 - color.B);
        }
    }

    updateLEDs();
}

KeypadFeedbackMode PadLedController::getFeedbackMode() {
    return currentFeedbackMode_;
}

// ── Brightness ──────────────────────────────────────────────────────────

void PadLedController::setBrightness(uint8_t bright) {
    brightness_ = bright;
    updateLEDs();
}

uint8_t PadLedController::getBrightness() {
    return brightness_;
}

// ── Animations (legacy stubs) ───────────────────────────────────────────

bool PadLedController::playAnimation(KeypadAnimationMode mode, KeypadAnimationParams params) {
    isAnimating_ = true;
    currentAnimMode_ = mode;
    currentAnimParams_ = params;
    // Legacy animation modes are stubs — real animations use PadAnimator
    isAnimating_ = false;
    return false;
}

bool PadLedController::playPadAnimation(uint8_t, KeypadAnimationParams) {
    return false;
}

void PadLedController::stopAnimation() {
    isAnimating_ = false;
    for (int i = 0; i < 16; i++) {
        padStates_[i].isAnimating = false;
    }
    updateLEDs();
}

bool PadLedController::isAnimationActive() {
    return isAnimating_;
}

// ── Callbacks ───────────────────────────────────────────────────────────

void PadLedController::setNoteOnCallback(KeypadNoteOnCallback callback) {
    noteOnCallback_ = callback;
}

void PadLedController::setAftertouchCallback(KeypadAftertouchCallback callback) {
    aftertouchCallback_ = callback;
}

void PadLedController::setAnimationCompleteCallback(KeypadAnimationCompleteCallback callback) {
    animationCompleteCallback_ = callback;
}

void PadLedController::setCustomColorCallback(KeypadCustomColorCallback callback) {
    customColorCallback_ = callback;
}

// ── Color calculation ───────────────────────────────────────────────────

static inline uint8_t mapRange(uint8_t x, uint8_t in_min, uint8_t in_max,
                                uint8_t out_min, uint8_t out_max) {
    return (uint8_t)((int)(x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

RgbColor PadLedController::calculatePadColor(uint8_t padIdx) {
    if (padIdx >= 16) return RgbColor(0, 0, 0);

    PadLedState& state = padStates_[padIdx];

    if (state.isAnimating) {
        return RgbColor(20, 10, 100);
    }

    // App-defined mode with custom callback
    if (currentFeedbackMode_ == KeypadFeedbackMode::APP_DEFINED && customColorCallback_) {
        RgbColor color = customColorCallback_(padIdx, state.isPressed, state.isPlaying);
        applyBrightness(color);
        return color;
    }

    // Velocity-based feedback
    if (currentFeedbackMode_ == KeypadFeedbackMode::VELOCITY && state.isPressed) {
        RgbColor color = state.pressedColor;
        uint8_t velocityBrightness = mapRange(state.velocity, 0, 127, 10, 255);
        color = color.Dim(velocityBrightness);
        applyBrightness(color);
        return color;
    }

    // Aftertouch feedback
    if (currentFeedbackMode_ == KeypadFeedbackMode::AFTERTOUCH && state.isPressed) {
        RgbColor color = state.pressedColor;
        uint8_t pressureBrightness = mapRange(state.pressure, 0, 127, 10, 255);
        color = color.Dim(pressureBrightness);
        applyBrightness(color);
        return color;
    }

    // Standard state-based colors
    RgbColor color;
    if (state.isPressed) {
        color = state.pressedColor;
    } else if (state.isPlaying && settings_ &&
               ((settings_->keypad.lightsOnNoteOn && !state.isPressed) ||
                (settings_->keypad.lightsOnNoteOff && !state.isPressed))) {
        color = state.playingColor;
    } else {
        color = state.defaultColor;
    }

    applyBrightness(color);
    return color;
}

void PadLedController::applyBrightness(RgbColor& color) {
    uint8_t settingsBri = settings_ ? (uint8_t)(settings_->RGBbrightness * 255) : 255;
    uint8_t effectiveBrightness = std::min(brightness_, settingsBri);
    color = color.Dim(effectiveBrightness);
}

int PadLedController::getPadFromMidiNote(uint8_t note) {
    for (int i = 0; i < 16; i++) {
        if (padStates_[i].midiNote == note) {
            return i;
        }
    }
    return -1;
}

} // namespace crosspad
