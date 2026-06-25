#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/led/LedTypes.hpp"
#include "crosspad/pad/Pad.hpp"
#include "crosspad/pad/IPadLogicHandler.hpp"
#include "crosspad/pad/RGBCallbackManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/midi/IMidiOutput.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

namespace crosspad {

/**
 * @brief Portable pad manager — input handling, MIDI, LED coordination.
 *
 * Manages pad interactions, note mapping, pad-logic-handler dispatch,
 * and coordinates PadLedController + PadAnimator. Uses IMidiOutput and
 * IEventBus for platform-independent MIDI output and event posting.
 */
class PadManager {
public:
    PadManager() = default;
    ~PadManager() = default;

    void init(PadLedController& ledController, PadAnimator& animator,
              IMidiOutput& midiOutput, IEventBus& eventBus,
              CrosspadSettings* settings, CrosspadStatus* status);
    void begin();
    void update();

    // Input suppression (replaces global forceButtonOff)
    void setInputSuppressed(bool suppressed) { inputSuppressed_ = suppressed; }
    bool isInputSuppressed() const { return inputSuppressed_; }
    void allNotesOff();

    // Physical pad interaction
    void handlePadPress(uint8_t padIdx, uint8_t velocity = 127);
    void handlePadRelease(uint8_t padIdx);
    void handlePadPressure(uint8_t padIdx, uint8_t pressure);
    // Like handlePadPressure but always drives the LED brightness by padIdx,
    // bypassing the note↔pad lookups (getPadForMidiNote can return 255 under
    // sampler kit configs; PadLedController.midiNote is unpopulated). Used by
    // the STM32 MIDI aftertouch route to make live pressure reach the RGB.
    void notifyPadPressure(uint8_t padIdx, uint8_t pressure);

    // Notification methods for updating LEDs directly
    void notifyNoteOn(uint8_t padIdx, uint8_t velocity, bool fromSampler = false);
    void notifyNoteOff(uint8_t padIdx, bool fromSampler = false);
    void notifySamplePlay(uint8_t padIdx, bool isPlaying);

    // MIDI event handling
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void handleMidiNoteOff(uint8_t channel, uint8_t note);
    void handleMidiAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // Sample playback
    void handleSamplePlay(uint8_t padIdx, bool isPlaying, uint8_t wavIdx = 0);
    void togglePadPlay(uint8_t padIdx);

    // Pad state queries
    bool isPadPressed(uint8_t padIdx);
    bool isPadPlaying(uint8_t padIdx);

    // Color management (forwarded to PadLedController)
    void setPadColor(uint8_t padIdx, RgbColor color);
    void setDefaultColor(RgbColor color);
    void setPressedColor(RgbColor color);
    void setPlayingColor(RgbColor color);
    void setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    void setDefaultColor(uint8_t r, uint8_t g, uint8_t b);
    void setPressedColor(uint8_t r, uint8_t g, uint8_t b);
    void setPlayingColor(uint8_t r, uint8_t g, uint8_t b);
    RgbColor getPadColor(uint8_t padIdx);
    RgbColor getPressedColor(uint8_t padIdx);
    RgbColor getPlayingColor(uint8_t padIdx);

    // Brightness
    void setBrightness(uint8_t brightness);
    uint8_t getBrightness();

    // Feedback mode
    void setFeedbackMode(KeypadFeedbackMode mode);
    KeypadFeedbackMode getFeedbackMode();

    // Animation
    bool playAnimation(KeypadAnimationMode mode, KeypadAnimationParams params);
    bool playPadAnimation(uint8_t padIdx, KeypadAnimationParams params);
    void stopAnimation();
    bool isAnimationActive();

    // Custom color callback
    void setCustomColorCallback(KeypadCustomColorCallback callback);

    // Pad config
    void loadPadConfig(Pad* pads, uint8_t count);
    void setPadNote(uint8_t padIdx, uint8_t note);
    uint8_t getPadNote(uint8_t padIdx);
    void setChordNote(uint8_t padIdx, uint8_t chordIdx, int8_t note);
    int8_t getChordNote(uint8_t padIdx, uint8_t chordIdx);
    void setPadChannel(uint8_t padIdx, uint8_t channel);
    uint8_t getPadChannel(uint8_t padIdx);

    // Note mapping
    void applyDefaultOffsetToPadNotes();
    uint8_t getMidiNoteForPad(uint8_t padIdx);
    uint8_t getPadForMidiNote(uint8_t note);

    // RGB Callback Manager
    RGBCallbackManager& getRGBCallbackManager() { return rgbCallbackManager_; }

    // Pad Logic Application Management
    void registerPadLogic(const std::string& appName, std::shared_ptr<IPadLogicHandler> handler);
    void setActivePadLogic(const std::string& appName);
    std::string getActivePadLogic() const;
    void unregisterPadLogic(const std::string& appName);
    std::vector<std::string> getRegisteredPadLogics() const;

    // Accessors
    PadLedController& getLedController() { return *ledController_; }
    PadAnimator& getAnimator() { return *animator_; }

private:
    PadLedController* ledController_ = nullptr;
    PadAnimator* animator_ = nullptr;
    IMidiOutput* midiOutput_ = nullptr;
    IEventBus* eventBus_ = nullptr;
    CrosspadSettings* settings_ = nullptr;
    CrosspadStatus* status_ = nullptr;

    // Internal state tracking
    bool padStates_[16] = {false};
    bool padPlayStates_[16] = {false};
    uint8_t padVelocity_[16] = {0};
    uint8_t padPressure_[16] = {0};
    uint8_t padNotes_[16] = {0};
    uint8_t padChannels_[16] = {0};
    int8_t padChordNotes_[16][8] = {};

    // Pad configuration
    Pad* pads_ = nullptr;
    uint8_t padCount_ = 0;

    // Pad Logic Application Management
    std::map<std::string, std::shared_ptr<IPadLogicHandler>> padLogicHandlers_;
    std::shared_ptr<IPadLogicHandler> activePadLogic_ = nullptr;
    std::string activePadLogicName_;

    // RGB Callback Manager
    RGBCallbackManager rgbCallbackManager_;

    // Input suppression flag
    bool inputSuppressed_ = false;

    // Thread safety
    mutable std::mutex stateMutex_;
    mutable std::mutex logicMutex_;

    // Helpers
    int getPadIndex(uint8_t midiNote);
    void updateLEDState(uint8_t padIdx);
};

/// Platform-injected singleton accessor
PadManager& getPadManager();

} // namespace crosspad
