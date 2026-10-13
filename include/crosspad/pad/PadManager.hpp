// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
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
    /// @p source says who pressed: a finger (PadManager, the default) or the
    /// loop recorder playing a take back (SequencerCallback). It travels with
    /// the bus events, and pad logic reads it with pressSource().
    /// @p performance: for the recorder's presses, how the hit sounded when it
    /// was recorded -- pad logic reads it with pressPerformance().
    void handlePadPress(uint8_t padIdx, uint8_t velocity = 127,
                        EventSource source = EventSource::PadManager,
                        PadPerformance performance = {});
    void handlePadRelease(uint8_t padIdx, EventSource source = EventSource::PadManager);
    /// A finger came onto @p padIdx (@p touching) and its velocity is still
    /// being measured, or it left without a hit (!@p touching). Passed to the
    /// active pad logic's onPadTouch(); a press or a release follows the way
    /// it always did. Nothing while input is suppressed.
    void handlePadTouch(uint8_t padIdx, bool touching);
    /// Called with every press a player makes -- whatever app's pad logic is
    /// active, never for the loop recorder's playback (SequencerCallback) --
    /// before it is handled. A platform whose pads come in by more ways than
    /// it can stamp one by one gives it the loop recorder's input here.
    using InputTap = void (*)(void* ctx, uint8_t padIdx, uint8_t velocity);
    void setInputTap(InputTap tap, void* ctx) { inputTap_ = tap; inputTapCtx_ = ctx; }
    /// Who made the last press or release of @p padIdx. Per pad, not per
    /// call: a take playing pad 3 while a finger holds pad 5 answers each
    /// right, which one shared flag set around the dispatch could not.
    EventSource pressSource(uint8_t padIdx) const {
        return padIdx < 16 ? pressSource_[padIdx].load(std::memory_order_acquire)
                           : EventSource::PadManager;
    }
    /// How the last press of @p padIdx was to sound, as handlePadPress() was
    /// told: what the loop recorder kept with a hit, for its own presses. A
    /// finger's press is plain here -- pad logic resolves those itself.
    PadPerformance pressPerformance(uint8_t padIdx) const {
        return padIdx < 16
            ? PadPerformance::unpack(pressPerformance_[padIdx].load(std::memory_order_acquire))
            : PadPerformance{};
    }
    /// What a press of @p padIdx would sound as under the active pad logic
    /// (IPadLogicHandler::performance()); @p velocity becomes what it would
    /// sound at. For the loop recorder, at the moment a hit comes in.
    PadPerformance performanceFor(uint8_t padIdx, uint8_t& velocity) const;
    void handlePadPressure(uint8_t padIdx, uint8_t pressure);
    // Like handlePadPressure but always drives the LED brightness by padIdx,
    // bypassing the note↔pad lookups (getPadForMidiNote can return 255 under
    // sampler kit configs; PadLedController.midiNote is unpopulated). Used by
    // the STM32 MIDI aftertouch route to make live pressure reach the RGB.
    void notifyPadPressure(uint8_t padIdx, uint8_t pressure);
    // Left/right balance of the press (IPadLogicHandler::kPanMin..kPanMax),
    // from the STM32's two cells per pad; reaches the active pad logic only —
    // no LED, no MIDI.
    void notifyPadPan(uint8_t padIdx, int8_t pan);
    int8_t getPadPan(uint8_t padIdx);

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
    void clearPadColor(uint8_t padIdx);   // drop custom-color override → default-managed
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
    /// Activate appName's logic. An empty name hands the pads to the base
    /// logic (see setBasePadLogic()) rather than leaving them dead. Blocks,
    /// before the old logic's onDeactivate(), until the pad callbacks other
    /// threads are running have returned (IPadLogicHandler's guarantee); so
    /// does every call below that deactivates a logic.
    void setActivePadLogic(const std::string& appName);
    /// Install the system instrument: the logic that owns the pads whenever no
    /// app has claimed them, so pads keep playing outside any app. Apps preempt
    /// it through pushPadLogic()/setActivePadLogic() and hand it back on
    /// teardown. Pass "" to remove it (pads go silent with no app again).
    ///
    /// The handler must stay registered for as long as it is the base — do not
    /// unregisterPadLogic() a base handler on app teardown, or the fallback
    /// resolves to nothing.
    void setBasePadLogic(const std::string& appName);
    std::string getBasePadLogic() const;
    std::string getActivePadLogic() const;
    void unregisterPadLogic(const std::string& appName);
    std::vector<std::string> getRegisteredPadLogics() const;
    /// The handler registered under appName, or null. For a logic that plays
    /// through another -- an app that owns the pads but keeps the base
    /// instrument sounding under them.
    std::shared_ptr<IPadLogicHandler> getPadLogic(const std::string& appName) const;
    /// Save the currently active pad logic on an internal stack, then activate
    /// appName. Pair with popPadLogic() — used by screens/apps that overlay a
    /// temporary logic and must hand control back on teardown.
    ///
    /// Caveat — unregister ordering: if the saved (pre-push) logic gets
    /// unregisterPadLogic()'d while it is sitting on the stack, popPadLogic()
    /// will end with NO active logic — setActivePadLogic()'s lookup-miss
    /// branch deactivates the current handler but activates nothing, since the
    /// saved name no longer resolves. Callers must popPadLogic() BEFORE
    /// unregistering their own handler, not after.
    ///
    /// Caveat — reentrancy: IPadLogicHandler::onActivate()/onDeactivate() run
    /// OUTSIDE logicMutex_ (see setActivePadLogic()). Calling pushPadLogic()
    /// or setActivePadLogic() from inside a handler's onActivate()/
    /// onDeactivate() re-enters PadManager's pad-logic state and can
    /// interleave activePadLogicName_/padLogicStack_ updates with the
    /// in-flight call. This is a pre-existing property of setActivePadLogic()
    /// but is more likely to bite via the stack API — don't do it. Nor from a
    /// pad callback (onPadPress() and the rest): see IPadLogicHandler for the
    /// deactivation guarantee and why a change from there breaks it.
    void pushPadLogic(const std::string& appName);
    /// Re-activate the logic saved by the matching pushPadLogic(). No-op when
    /// the stack is empty. Same unregister-ordering and reentrancy caveats as
    /// pushPadLogic() apply — see its doc comment.
    void popPadLogic();

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
    std::atomic<EventSource> pressSource_[16] = {};
    std::atomic<uint8_t>     pressPerformance_[16] = {};   ///< PadPerformance::pack()
    uint8_t padPressure_[16] = {0};
    int8_t padPan_[16] = {0};
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
    std::string basePadLogicName_;   ///< system instrument; owns pads when no app does
    std::vector<std::string> padLogicStack_;

    // RGB Callback Manager
    RGBCallbackManager rgbCallbackManager_;

    // Input suppression flag
    bool inputSuppressed_ = false;
    InputTap inputTap_ = nullptr;
    void* inputTapCtx_ = nullptr;

    // Thread safety
    mutable std::mutex stateMutex_;
    mutable std::mutex logicMutex_;

    // Pad-logic callbacks in flight on any thread (under logicMutex_). A
    // deactivation takes the handler out of dispatch, then waits for these
    // before onDeactivate(): see IPadLogicHandler.
    int dispatchInFlight_ = 0;
    std::condition_variable dispatchIdle_;

    // The active handler for one dispatch, counted in flight while it lives;
    // null (and not counted) when no pad logic is active.
    class DispatchScope {
    public:
        explicit DispatchScope(PadManager& pm);
        ~DispatchScope();
        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;
        std::shared_ptr<IPadLogicHandler> handler;
    private:
        PadManager& pm_;
    };
    // Under logicMutex_ (released while waiting): returns once no pad-logic
    // callback runs on another thread.
    void waitDispatchIdle(std::unique_lock<std::mutex>& lock);

    // Helpers
    int getPadIndex(uint8_t midiNote);
    void updateLEDState(uint8_t padIdx);
};

/// Platform-injected singleton accessor
PadManager& getPadManager();

} // namespace crosspad
