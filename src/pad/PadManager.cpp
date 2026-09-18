#include "crosspad/pad/PadManager.hpp"
#include <cstdio>

namespace crosspad {

void PadManager::init(PadLedController& ledController, PadAnimator& animator,
                       IMidiOutput& midiOutput, IEventBus& eventBus,
                       CrosspadSettings* settings, CrosspadStatus* status) {
    ledController_ = &ledController;
    animator_ = &animator;
    midiOutput_ = &midiOutput;
    eventBus_ = &eventBus;
    settings_ = settings;
    status_ = status;
}

void PadManager::begin() {
    // Initialize the LED controller (subscribes EventBus, initial LED update)
    if (ledController_) {
        ledController_->begin();
    }

    // Set initial pad notes (offset from status->keyOffset, default C2=36)
    uint8_t offset = status_ ? status_->keyOffset : 36;
    for (int i = 0; i < 16; i++) {
        padNotes_[i] = i + offset;
    }
}

void PadManager::update() {
    // Tick PadAnimator first; when active it sets isAnimating on PadLedController
    if (animator_) {
        animator_->update();
    }

    if (ledController_) {
        ledController_->update();
    }
}

// ── Input suppression ─────────────────────────────────────────────────

void PadManager::allNotesOff() {
    for (uint8_t i = 0; i < 16; i++) {
        bool wasPressed;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            wasPressed = padStates_[i];
        }
        if (wasPressed) {
            handlePadRelease(i);
        }
    }
}

// ── Physical pad interaction ────────────────────────────────────────────

void PadManager::handlePadPress(uint8_t padIdx, uint8_t velocity) {
    if (padIdx >= 16) return;
    if (inputSuppressed_) return;

    // Get active pad logic handler (thread-safe read)
    std::shared_ptr<IPadLogicHandler> handler;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        handler = activePadLogic_;
    }

    // Update internal state (thread-safe write)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padStates_[padIdx] = true;
        padVelocity_[padIdx] = velocity;
    }

    // Check if there's an active pad logic handler
    if (handler) {
        handler->onPadPress(*this, padIdx, velocity);
        return;
    }

    // Default behavior
    if (ledController_) {
        ledController_->notifyNoteOn(padIdx, velocity);
    }

    // Send MIDI message if needed
    if (settings_ && settings_->keypad.enableKeypad && !settings_->keypad.sendCC) {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        if (midiOutput_) {
            midiOutput_->sendNoteOn(note, channel);
        }
    }

    // Always send through EventBus
    if (eventBus_) {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        eventBus_->postNoteOn(channel, note, velocity, EventSource::PadManager);
        eventBus_->postPadPressed(padIdx, true, velocity, EventSource::PadManager);
    }
}

void PadManager::handlePadRelease(uint8_t padIdx) {
    if (padIdx >= 16) return;

    std::shared_ptr<IPadLogicHandler> handler;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        handler = activePadLogic_;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padStates_[padIdx] = false;
    }

    if (handler) {
        handler->onPadRelease(*this, padIdx);
        return;
    }

    if (ledController_) {
        ledController_->notifyNoteOff(padIdx);
    }

    if (settings_ && settings_->keypad.enableKeypad && !settings_->keypad.sendCC) {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        if (midiOutput_) {
            midiOutput_->sendNoteOff(note, channel);
        }

        if (!settings_->keypad.noteOffOnRelease) return;
    }

    if (eventBus_) {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        eventBus_->postNoteOff(channel, note, 0, EventSource::PadManager);
        eventBus_->postPadReleased(padIdx, EventSource::PadManager);
    }
}

void PadManager::handlePadPressure(uint8_t padIdx, uint8_t pressure) {
    if (padIdx >= 16) return;

    std::shared_ptr<IPadLogicHandler> handler;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        handler = activePadLogic_;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPressure_[padIdx] = pressure;
    }

    if (handler) {
        handler->onPadPressure(*this, padIdx, pressure);
        return;
    }

    if (ledController_) {
        ledController_->handleAftertouch(0, getMidiNoteForPad(padIdx), pressure);
    }
}

void PadManager::notifyPadPressure(uint8_t padIdx, uint8_t pressure) {
    if (padIdx >= 16) return;

    std::shared_ptr<IPadLogicHandler> handler;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        handler = activePadLogic_;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPressure_[padIdx] = pressure;
    }

    // App pad logic still sees pressure (e.g. sampler choke/effects)...
    if (handler) {
        handler->onPadPressure(*this, padIdx, pressure);
    }
    // ...but the LED brightness is ALWAYS driven here by padIdx, so live
    // pressure reaches the RGB regardless of which app is active (mirrors how
    // handlePadPress drives notifyNoteOn unconditionally).
    if (ledController_) {
        ledController_->notifyAftertouch(padIdx, pressure);
    }
}

void PadManager::notifyPadPan(uint8_t padIdx, int8_t pan) {
    if (padIdx >= 16) return;

    std::shared_ptr<IPadLogicHandler> handler;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        handler = activePadLogic_;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPan_[padIdx] = pan;
    }
    if (handler) {
        handler->onPadPan(*this, padIdx, pan);
    }
}

int8_t PadManager::getPadPan(uint8_t padIdx) {
    if (padIdx >= 16) return 0;
    std::lock_guard<std::mutex> lock(stateMutex_);
    return padPan_[padIdx];
}

// ── MIDI event handling ─────────────────────────────────────────────────

void PadManager::handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // MIDI convention: a note-on with velocity 0 is a note-off. Route it there
    // so it clears the pad press state instead of lighting the pad.
    if (velocity == 0) {
        handleMidiNoteOff(channel, note);
        return;
    }
    uint8_t padIdx = getPadForMidiNote(note);
    if (padIdx < 16) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            padStates_[padIdx] = true;
            padVelocity_[padIdx] = velocity;
        }
        if (ledController_) {
            ledController_->notifyNoteOn(padIdx, velocity);
        }
    }
}

void PadManager::handleMidiNoteOff(uint8_t channel, uint8_t note) {
    uint8_t padIdx = getPadForMidiNote(note);
    if (padIdx < 16) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            padStates_[padIdx] = false;
        }
        if (ledController_) {
            ledController_->notifyNoteOff(padIdx);
        }
    }
}

void PadManager::handleMidiAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) {
    uint8_t padIdx = getPadForMidiNote(note);
    if (padIdx < 16) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            padPressure_[padIdx] = pressure;
        }
        if (ledController_) {
            ledController_->handleAftertouch(channel, note, pressure);
        }
    }
}

// ── Sample playback ─────────────────────────────────────────────────────

void PadManager::handleSamplePlay(uint8_t padIdx, bool isPlaying, uint8_t wavIdx) {
    if (padIdx >= 16) return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPlayStates_[padIdx] = isPlaying;
    }
    if (ledController_) {
        ledController_->notifyNotePlay(padIdx, isPlaying);
    }
}

void PadManager::togglePadPlay(uint8_t padIdx) {
    if (padIdx >= 16) return;

    bool currentlyPlaying;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentlyPlaying = padPlayStates_[padIdx];
    }

    if (currentlyPlaying) {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        if (eventBus_) {
            eventBus_->postNoteOff(channel, note, 0, EventSource::PadManager);
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPlayStates_[padIdx] = false;
    } else {
        uint8_t note = getMidiNoteForPad(padIdx);
        uint8_t channel = getPadChannel(padIdx);
        if (eventBus_) {
            eventBus_->postNoteOn(channel, note, 127, EventSource::PadManager);
        }
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPlayStates_[padIdx] = true;
    }
}

// ── Pad state queries ───────────────────────────────────────────────────

bool PadManager::isPadPressed(uint8_t padIdx) {
    if (padIdx >= 16) return false;
    std::lock_guard<std::mutex> lock(stateMutex_);
    return padStates_[padIdx];
}

bool PadManager::isPadPlaying(uint8_t padIdx) {
    if (padIdx >= 16) return false;
    std::lock_guard<std::mutex> lock(stateMutex_);
    return padPlayStates_[padIdx];
}

// ── Color forwarding to PadLedController ────────────────────────────────

void PadManager::setPadColor(uint8_t padIdx, RgbColor color) {
    if (ledController_) ledController_->setPadColor(padIdx, color);
}
void PadManager::setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b) {
    if (ledController_) ledController_->setPadColor(padIdx, r, g, b);
}
void PadManager::clearPadColor(uint8_t padIdx) {
    if (ledController_) ledController_->clearPadColor(padIdx);
}
void PadManager::setDefaultColor(RgbColor color) {
    if (ledController_) ledController_->setDefaultColor(color);
}
void PadManager::setDefaultColor(uint8_t r, uint8_t g, uint8_t b) {
    if (ledController_) ledController_->setDefaultColor(RgbColor(r, g, b));
}
void PadManager::setPressedColor(RgbColor color) {
    if (ledController_) ledController_->setPressedColor(color);
}
void PadManager::setPressedColor(uint8_t r, uint8_t g, uint8_t b) {
    if (ledController_) ledController_->setPressedColor(RgbColor(r, g, b));
}
void PadManager::setPlayingColor(RgbColor color) {
    if (ledController_) ledController_->setPlayingColor(color);
}
void PadManager::setPlayingColor(uint8_t r, uint8_t g, uint8_t b) {
    if (ledController_) ledController_->setPlayingColor(RgbColor(r, g, b));
}
RgbColor PadManager::getPadColor(uint8_t padIdx) {
    return ledController_ ? ledController_->getPadColor(padIdx) : RgbColor(0, 0, 0);
}
RgbColor PadManager::getPressedColor(uint8_t padIdx) {
    return ledController_ ? ledController_->getPressedColor(padIdx) : RgbColor(0, 0, 0);
}
RgbColor PadManager::getPlayingColor(uint8_t padIdx) {
    return ledController_ ? ledController_->getPlayingColor(padIdx) : RgbColor(0, 0, 0);
}

// ── Brightness ──────────────────────────────────────────────────────────

void PadManager::setBrightness(uint8_t brightness) {
    if (ledController_) ledController_->setBrightness(brightness);
}
uint8_t PadManager::getBrightness() {
    return ledController_ ? ledController_->getBrightness() : 255;
}

// ── Feedback mode ───────────────────────────────────────────────────────

void PadManager::setFeedbackMode(KeypadFeedbackMode mode) {
    if (ledController_) ledController_->setFeedbackMode(mode);
}
KeypadFeedbackMode PadManager::getFeedbackMode() {
    return ledController_ ? ledController_->getFeedbackMode() : KeypadFeedbackMode::STANDARD;
}

void PadManager::setCustomColorCallback(KeypadCustomColorCallback callback) {
    if (ledController_) ledController_->setCustomColorCallback(callback);
}

// ── Pad configuration ───────────────────────────────────────────────────

void PadManager::loadPadConfig(Pad* padArray, uint8_t count) {
    pads_ = padArray;
    padCount_ = count;

    for (int i = 0; i < count && i < 16; i++) {
        Pad& pad = pads_[i];

        uint8_t r = (pad.color >> 16) & 0xFF;
        uint8_t g = (pad.color >> 8) & 0xFF;
        uint8_t b = pad.color & 0xFF;
        RgbColor padColor(r, g, b);

        uint8_t pressedR = (pad.color_pressed >> 16) & 0xFF;
        uint8_t pressedG = (pad.color_pressed >> 8) & 0xFF;
        uint8_t pressedB = pad.color_pressed & 0xFF;
        RgbColor pressedColor(pressedR, pressedG, pressedB);

        if (ledController_) {
            ledController_->setPadColor(pad.number, padColor);
            ledController_->setPadPressedColor(pad.number, pressedColor);
        }

        padNotes_[pad.number] = pad.note;
    }
}

void PadManager::setPadNote(uint8_t padIdx, uint8_t note) {
    if (padIdx < 16) {
        padNotes_[padIdx] = note;
        if (pads_ && padIdx < padCount_) {
            pads_[padIdx].note = note;
        }
    }
}

uint8_t PadManager::getPadNote(uint8_t padIdx) {
    if (padIdx < 16) {
        if (pads_ && padIdx < padCount_) {
            return pads_[padIdx].note;
        }
        return padNotes_[padIdx];
    }
    return 0;
}

void PadManager::setChordNote(uint8_t padIdx, uint8_t chordIdx, int8_t note) {
    if (padIdx < 16 && chordIdx < 8) {
        padChordNotes_[padIdx][chordIdx] = note;
    }
}

int8_t PadManager::getChordNote(uint8_t padIdx, uint8_t chordIdx) {
    if (padIdx < 16 && chordIdx < 8) {
        return padChordNotes_[padIdx][chordIdx];
    }
    return -1;
}

void PadManager::setPadChannel(uint8_t padIdx, uint8_t channel) {
    if (padIdx < 16) {
        padChannels_[padIdx] = channel;
    }
}

uint8_t PadManager::getPadChannel(uint8_t padIdx) {
    return (padIdx < 16) ? padChannels_[padIdx] : 0;
}

// ── Note mapping ────────────────────────────────────────────────────────

void PadManager::applyDefaultOffsetToPadNotes() {
    uint8_t offset = status_ ? status_->keyOffset : 36;
    for (int i = 0; i < 16; i++) {
        padNotes_[i] = i + offset;
        if (pads_ && i < padCount_) {
            pads_[i].note = i + offset;
        }
    }
}

uint8_t PadManager::getMidiNoteForPad(uint8_t padIdx) {
    if (padIdx >= 16) return 0;
    if (pads_ && padIdx < padCount_) {
        return pads_[padIdx].note;
    }
    return padNotes_[padIdx];
}

uint8_t PadManager::getPadForMidiNote(uint8_t note) {
    if (pads_) {
        for (int i = 0; i < padCount_ && i < 16; i++) {
            if (pads_[i].note == note) {
                return pads_[i].number;
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        if (padNotes_[i] == note) {
            return i;
        }
    }
    return 255;
}

int PadManager::getPadIndex(uint8_t midiNote) {
    return getPadForMidiNote(midiNote);
}

void PadManager::updateLEDState(uint8_t padIdx) {
    if (padIdx >= 16 || !ledController_) return;

    bool pressed, playing;
    uint8_t velocity;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        pressed = padStates_[padIdx];
        playing = padPlayStates_[padIdx];
        velocity = padVelocity_[padIdx];
    }

    if (pressed) {
        ledController_->notifyNoteOn(padIdx, velocity);
    } else if (playing) {
        ledController_->notifyNotePlay(padIdx, true);
    } else {
        ledController_->notifyNoteOff(padIdx);
    }
}

// ── Notification methods ────────────────────────────────────────────────

void PadManager::notifyNoteOn(uint8_t padIdx, uint8_t velocity, bool fromSampler) {
    if (padIdx >= 16) return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padStates_[padIdx] = true;
        padVelocity_[padIdx] = velocity;
    }
    if (ledController_) {
        ledController_->notifyNoteOn(padIdx, velocity);
    }
}

void PadManager::notifyNoteOff(uint8_t padIdx, bool fromSampler) {
    if (padIdx >= 16) return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padStates_[padIdx] = false;
    }
    if (ledController_) {
        ledController_->notifyNoteOff(padIdx);
    }
}

void PadManager::notifySamplePlay(uint8_t padIdx, bool isPlaying) {
    if (padIdx >= 16) return;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        padPlayStates_[padIdx] = isPlaying;
    }
    if (ledController_) {
        ledController_->notifyNotePlay(padIdx, isPlaying);
    }
}

// ── Pad Logic Application Management ────────────────────────────────────

void PadManager::registerPadLogic(const std::string& appName, std::shared_ptr<IPadLogicHandler> handler) {
    if (!handler) {
        printf("PadManager: Cannot register null handler for app '%s'\n", appName.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(logicMutex_);
    padLogicHandlers_[appName] = handler;
    printf("PadManager: Registered pad logic for app '%s'\n", appName.c_str());
}

void PadManager::setActivePadLogic(const std::string& appName) {
    std::unique_lock<std::mutex> lock(logicMutex_);

    // An empty name means "no app owns the pads" — which is not the same as
    // "the pads go dead". When a base instrument is installed it takes over,
    // so pads keep playing everywhere in the system until an app preempts it.
    const std::string target = appName.empty() ? basePadLogicName_ : appName;

    if (activePadLogic_ && !activePadLogicName_.empty()) {
        printf("PadManager: Deactivating pad logic '%s'\n", activePadLogicName_.c_str());
        auto oldLogic = activePadLogic_;
        activePadLogic_ = nullptr;
        activePadLogicName_ = "";

        // Unlock before calling user code to avoid deadlock
        lock.unlock();
        oldLogic->onDeactivate(*this);
        lock.lock();
    } else {
        activePadLogic_ = nullptr;
        activePadLogicName_ = "";
    }

    if (target.empty()) {
        printf("PadManager: No app specified, using default pad logic\n");
        return;
    }

    auto it = padLogicHandlers_.find(target);
    if (it != padLogicHandlers_.end()) {
        activePadLogic_ = it->second;
        activePadLogicName_ = target;
        printf("PadManager: Activating pad logic '%s'\n", target.c_str());

        auto newLogic = activePadLogic_;
        lock.unlock();
        newLogic->onActivate(*this);
        lock.lock();
    } else {
        printf("PadManager: App '%s' not found, using default pad logic\n", target.c_str());
    }
}

void PadManager::setBasePadLogic(const std::string& appName) {
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        if (basePadLogicName_ == appName) return;
        basePadLogicName_ = appName;
    }
    // Take effect now if nothing else currently owns the pads.
    if (getActivePadLogic().empty()) {
        setActivePadLogic("");
    }
}

std::string PadManager::getBasePadLogic() const {
    std::lock_guard<std::mutex> lock(logicMutex_);
    return basePadLogicName_;
}

std::string PadManager::getActivePadLogic() const {
    std::lock_guard<std::mutex> lock(logicMutex_);
    return activePadLogicName_;
}

void PadManager::unregisterPadLogic(const std::string& appName) {
    std::unique_lock<std::mutex> lock(logicMutex_);

    if (activePadLogicName_ == appName && activePadLogic_) {
        auto oldLogic = activePadLogic_;
        activePadLogic_ = nullptr;
        activePadLogicName_ = "";

        lock.unlock();
        oldLogic->onDeactivate(*this);
        lock.lock();
    }

    auto it = padLogicHandlers_.find(appName);
    if (it != padLogicHandlers_.end()) {
        padLogicHandlers_.erase(it);
        printf("PadManager: Unregistered pad logic for app '%s'\n", appName.c_str());
    }
}

std::vector<std::string> PadManager::getRegisteredPadLogics() const {
    std::lock_guard<std::mutex> lock(logicMutex_);
    std::vector<std::string> apps;
    for (const auto& pair : padLogicHandlers_) {
        apps.push_back(pair.first);
    }
    return apps;
}

void PadManager::pushPadLogic(const std::string& appName) {
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        padLogicStack_.push_back(activePadLogicName_);
    }
    setActivePadLogic(appName);
}

void PadManager::popPadLogic() {
    std::string prev;
    {
        std::lock_guard<std::mutex> lock(logicMutex_);
        if (padLogicStack_.empty()) return;
        prev = padLogicStack_.back();
        padLogicStack_.pop_back();
    }
    setActivePadLogic(prev);
}

} // namespace crosspad
