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
    // Initialize the LED strip hardware (creates mutex, starts RMT peripheral)
    if (ledStrip_) {
        ledStrip_->begin();
    }

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
    // Coalesced mode: flush at most one refresh() per tick. On hardware a
    // refresh is an RMT transmission that interferes with I2S audio DMA
    // (random output clicks), so per-note-event refreshes must not reach the
    // strip directly. The unlocked dirty read is benign — a missed flush is
    // caught on the next tick.
    if (!coalesceRefresh_ || !ledStrip_) return;
    // The playing states the render recorded since the last tick: painted
    // here, under the lock the render must never wait for.
    if (repaint_.exchange(false, std::memory_order_acq_rel)) updateLEDs();
    if (!ledsDirty_) return;
    ledStrip_->lock();
    if (ledsDirty_) {
        ledsDirty_ = false;
        ledStrip_->refresh();
    }
    ledStrip_->unlock();
}

// ── LED update methods ──────────────────────────────────────────────────

// Push buffered pixels, or just mark them dirty for the next update() tick
// when coalescing is on. Caller must hold the strip lock.
void PadLedController::refreshOrMarkDirty() {
    if (coalesceRefresh_) {
        ledsDirty_ = true;
    } else {
        ledStrip_->refresh();
    }
}

void PadLedController::updateLEDs() {
    if (isAnimating_ || !ledStrip_) return;
    ledStrip_->lock();
    if (isAnimating_) { ledStrip_->unlock(); return; } // double-check after lock
    for (int i = 0; i < padCount_; i++) {
        RgbColor color = calculatePadColor(i);
        ledStrip_->setPixel(i, color);
    }
    refreshOrMarkDirty();
    ledStrip_->unlock();
}

void PadLedController::updateSingleLED(uint8_t padIdx) {
    if (padIdx >= padCount_ || !ledStrip_) return;
    if (isAnimating_) return;
    ledStrip_->lock();
    if (isAnimating_) { ledStrip_->unlock(); return; }
    RgbColor color = calculatePadColor(padIdx);
    ledStrip_->setPixel(padIdx, color);
    refreshOrMarkDirty();
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

void PadLedController::notifyAftertouch(uint8_t padIdx, uint8_t pressure) {
    if (padIdx >= 16) return;

    padStates_[padIdx].pressure = pressure;

    if (aftertouchCallback_) {
        aftertouchCallback_(padIdx, pressure);
    }

    // Repaint only when live pressure actually drives the visual (AFTERTOUCH
    // mode, pad held). updateSingleLED keeps it cheap at the ~60 Hz STM rate.
    if (currentFeedbackMode_ == KeypadFeedbackMode::AFTERTOUCH &&
        padStates_[padIdx].isPressed) {
        updateSingleLED(padIdx);
    }
}

void PadLedController::notifyNotePlay(uint8_t padIdx, bool isPlaying) {
    if (padIdx >= 16) return;

    padStates_[padIdx].isPlaying = isPlaying;

    if (padStates_[padIdx].isPressed) return;
    // The engine reports voices from its render: no strip lock there (the
    // worker holds it for a whole RMT frame, ~0.75 ms), the next update()
    // paints. Coalesced, that tick is when the frame would go out anyway.
    if (coalesceRefresh_) {
        repaint_.store(true, std::memory_order_release);
        return;
    }
    updateLEDs();
}

void PadLedController::notifyNoteDimmed(uint8_t padIdx, bool dimmed) {
    if (padIdx >= 16) return;
    if (padStates_[padIdx].isDimmed == dimmed) return;
    padStates_[padIdx].isDimmed = dimmed;
    if (padStates_[padIdx].isPressed) return;
    if (coalesceRefresh_) {
        repaint_.store(true, std::memory_order_release);
        return;
    }
    updateLEDs();
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

void PadLedController::clearPadColor(uint8_t padIdx) {
    if (padIdx >= 16) return;
    // Return to default-managed state. While hasCustomColor is true the pad is
    // pinned to defaultColor (calculatePadColor's AFTERTOUCH branch shows it
    // instead of pressedColor), so a screen that painted pads via setPadColor
    // must clear the flag on teardown or it hijacks the normal pressed-LED feedback.
    padStates_[padIdx].hasCustomColor = false;
    padStates_[padIdx].defaultColor   = defaultColor_;
    padStates_[padIdx].pressedColor   = pressedColor_;
    padStates_[padIdx].playingColor   = playingColor_;
    if (!padStates_[padIdx].isPressed && !padStates_[padIdx].isPlaying) {
        updateSingleLED(padIdx);
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

// ── Animation control ───────────────────────────────────────────────────

void PadLedController::stopAnimation() {
    isAnimating_ = false;
    for (int i = 0; i < 16; i++) {
        padStates_[i].isAnimating = false;
    }
    updateLEDs();
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

namespace {

RgbColor dimmedOf(const RgbColor& c) {
    const int peak = std::max({c.R, c.G, c.B});
    if (peak == 0) return c;
    const int want = std::clamp(peak / 2, (int)PadLedController::kDimmedMinPeak,
                                (int)PadLedController::kDimmedMaxPeak);
    return c.Dim(static_cast<uint8_t>(std::min(255, want * 255 / peak)));
}

} // namespace

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

    // Aftertouch feedback — modulate brightness by live pressure. Use the pad's
    // PRESSED color (kit color_pressed): it contrasts with the resting color,
    // so the pressure gradient stays clearly visible on the RGB grid.
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
    } else if (state.isDimmed) {
        color = dimmedOf(state.playingColor);
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
    uint8_t settingsBri = settings_ ? settings_->RGBbrightness : 255;
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
