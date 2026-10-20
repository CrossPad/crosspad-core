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

    /// Wire the manager to the LED controller, animator, MIDI output, event
    /// bus, settings and status. The platform calls it once at start-up.
    void init(PadLedController& ledController, PadAnimator& animator,
              IMidiOutput& midiOutput, IEventBus& eventBus,
              CrosspadSettings* settings, CrosspadStatus* status);
    /// Start the LED controller and give each pad its default note (the key
    /// offset + the pad index). The platform calls it once, after init().
    void begin();
    /// Advance the pad animator and the LED controller. The platform calls it
    /// periodically.
    void update();

    /// While suppressed, pad presses and touches are ignored; releases still
    /// go through.
    void setInputSuppressed(bool suppressed) { inputSuppressed_ = suppressed; }
    /// Whether pad presses are being ignored (setInputSuppressed()).
    bool isInputSuppressed() const { return inputSuppressed_; }
    /// Release every pad that is held, as handlePadRelease() would.
    void allNotesOff();

    // ── Pad input (fed by the platform) ─────────────────────────────────
    /// A pad press (padIdx 0-15, velocity 0-127). The active pad logic gets
    /// onPadPress(); with none, the pad lights and the note goes to MIDI and
    /// the event bus.
    ///
    /// @p source says who pressed: a finger (PadManager, the default) or the
    /// loop recorder playing a take back (SequencerCallback). It travels with
    /// the bus events, and pad logic reads it with pressSource().
    /// @p performance: for the recorder's presses, how the hit sounded when it
    /// was recorded -- pad logic reads it with pressPerformance().
    void handlePadPress(uint8_t padIdx, uint8_t velocity = 127,
                        EventSource source = EventSource::PadManager,
                        PadPerformance performance = {});
    /// A pad release (padIdx 0-15). The active pad logic gets onPadRelease();
    /// with none, the pad's LED and note are released.
    void handlePadRelease(uint8_t padIdx, EventSource source = EventSource::PadManager);
    /// A finger came onto @p padIdx (@p touching) and its velocity is still
    /// being measured, or it left without a hit (!@p touching). Passed to the
    /// active pad logic's onPadTouch(); a press or a release follows the way
    /// it always did. Nothing while input is suppressed.
    void handlePadTouch(uint8_t padIdx, bool touching);
    /// Called with every press a player makes -- whatever app's pad logic is
    /// active, never for the loop recorder's playback (SequencerCallback) nor
    /// for an app playing the pads itself (App, a kit's preview) -- before it
    /// is handled. A platform whose pads come in by more ways than
    /// it can stamp one by one gives it the loop recorder's input here.
    using InputTap = void (*)(void* ctx, uint8_t padIdx, uint8_t velocity);
    /// Install the InputTap (one at a time; nullptr removes it).
    void setInputTap(InputTap tap, void* ctx) { inputTap_ = tap; inputTapCtx_ = ctx; }
    /// Who made the last press or release of @p padIdx. Per pad, not per
    /// call: a take playing pad 3 while a finger holds pad 5 answers each
    /// right.
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
    /// Pad pressure (aftertouch, 0-127) for the active pad logic's
    /// onPadPressure(). Pressure from the pad hardware comes in through
    /// notifyPadPressure(), which also drives the pad's LED.
    void handlePadPressure(uint8_t padIdx, uint8_t pressure);
    /// Pad pressure (0-127) from the pad hardware. The active pad logic gets
    /// onPadPressure(), and the pad's LED follows the pressure by pad index
    /// whatever app is active.
    void notifyPadPressure(uint8_t padIdx, uint8_t pressure);
    /// Left/right balance of the press (IPadLogicHandler::kPanMin..kPanMax),
    /// from the two cells under each pad. Reaches the active pad logic's
    /// onPadPan() only: no LED, no MIDI.
    void notifyPadPan(uint8_t padIdx, int8_t pan);
    /// The last pan notifyPadPan() delivered for @p padIdx; 0 when centred.
    int8_t getPadPan(uint8_t padIdx);

    // ── LED state without dispatch ──────────────────────────────────────
    /// Mark the pad as held and light it, without pad logic, MIDI or bus
    /// events. @p fromSampler is ignored.
    void notifyNoteOn(uint8_t padIdx, uint8_t velocity, bool fromSampler = false);
    /// Mark the pad as released and repaint it, without pad logic, MIDI or
    /// bus events. @p fromSampler is ignored.
    void notifyNoteOff(uint8_t padIdx, bool fromSampler = false);
    /// Mark the pad as playing (a sample sounding on it) or not, and repaint.
    void notifySamplePlay(uint8_t padIdx, bool isPlaying);

    // ── MIDI input ──────────────────────────────────────────────────────
    /// An incoming Note On lights the pad mapped to @p note; velocity 0 is
    /// treated as a Note Off. No pad logic, MIDI out or bus events.
    void handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    /// An incoming Note Off releases the LED of the pad mapped to @p note.
    void handleMidiNoteOff(uint8_t channel, uint8_t note);
    /// Incoming polyphonic aftertouch for the pad mapped to @p note.
    void handleMidiAftertouch(uint8_t channel, uint8_t note, uint8_t pressure);

    // ── Sample playback ─────────────────────────────────────────────────
    /// Same as notifySamplePlay(); @p wavIdx is ignored.
    void handleSamplePlay(uint8_t padIdx, bool isPlaying, uint8_t wavIdx = 0);
    /// Start or stop the pad's note through the event bus: posts a Note On
    /// when the pad is not playing, a Note Off when it is.
    void togglePadPlay(uint8_t padIdx);

    // ── Pad state ───────────────────────────────────────────────────────
    /// Whether the pad is held now.
    bool isPadPressed(uint8_t padIdx);
    /// Whether a sample is sounding on the pad now.
    bool isPadPlaying(uint8_t padIdx);

    // ── Colours (forwarded to PadLedController) ─────────────────────────
    /// Give one pad a resting colour of its own. From then on the global
    /// colours below no longer change that pad, until clearPadColor().
    void setPadColor(uint8_t padIdx, RgbColor color);
    /// Resting colour of every pad without a colour of its own.
    void setDefaultColor(RgbColor color);
    /// Colour of a held pad, for every pad without a colour of its own.
    void setPressedColor(RgbColor color);
    /// Colour of a pad whose sample is sounding, for every pad without a
    /// colour of its own.
    void setPlayingColor(RgbColor color);
    /// setPadColor() from 8-bit red, green and blue.
    void setPadColor(uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b);
    /// Drop the pad's own colour: it follows the global colours again.
    void clearPadColor(uint8_t padIdx);
    /// setDefaultColor() from 8-bit red, green and blue.
    void setDefaultColor(uint8_t r, uint8_t g, uint8_t b);
    /// setPressedColor() from 8-bit red, green and blue.
    void setPressedColor(uint8_t r, uint8_t g, uint8_t b);
    /// setPlayingColor() from 8-bit red, green and blue.
    void setPlayingColor(uint8_t r, uint8_t g, uint8_t b);
    /// The pad's resting colour.
    RgbColor getPadColor(uint8_t padIdx);
    /// The pad's colour while held.
    RgbColor getPressedColor(uint8_t padIdx);
    /// The pad's colour while its sample sounds.
    RgbColor getPlayingColor(uint8_t padIdx);

    /// Pad LED brightness, 0-255. The lower of this and the LED brightness in
    /// Settings applies.
    void setBrightness(uint8_t brightness);
    /// The brightness set with setBrightness().
    uint8_t getBrightness();

    /// How the pads light when pressed (see KeypadFeedbackMode).
    void setFeedbackMode(KeypadFeedbackMode mode);
    /// The current feedback mode.
    KeypadFeedbackMode getFeedbackMode();

    /// The function that colours every pad in KeypadFeedbackMode::APP_DEFINED.
    void setCustomColorCallback(KeypadCustomColorCallback callback);

    // ── Pad configuration ───────────────────────────────────────────────
    /// Take a kit's pads: each pad's colour, pressed colour and note. The
    /// array is kept by pointer, so it must stay alive while it is in use.
    void loadPadConfig(Pad* pads, uint8_t count);
    /// Set the MIDI note the pad plays.
    void setPadNote(uint8_t padIdx, uint8_t note);
    /// The MIDI note the pad plays.
    uint8_t getPadNote(uint8_t padIdx);
    /// Set note @p chordIdx (0-7) of the pad's chord.
    void setChordNote(uint8_t padIdx, uint8_t chordIdx, int8_t note);
    /// Note @p chordIdx (0-7) of the pad's chord; -1 when an index is out of
    /// range.
    int8_t getChordNote(uint8_t padIdx, uint8_t chordIdx);
    /// Set the MIDI channel the pad sends on (0-based, 0-15).
    void setPadChannel(uint8_t padIdx, uint8_t channel);
    /// The MIDI channel the pad sends on (0-based).
    uint8_t getPadChannel(uint8_t padIdx);

    // ── Note mapping ────────────────────────────────────────────────────
    /// Reset every pad's note to the key offset + the pad index.
    void applyDefaultOffsetToPadNotes();
    /// The MIDI note pad @p padIdx plays.
    uint8_t getMidiNoteForPad(uint8_t padIdx);
    /// The pad that plays @p note, or 255 when no pad does.
    uint8_t getPadForMidiNote(uint8_t note);

    /// Named callbacks to run when the pad colours need repainting.
    RGBCallbackManager& getRGBCallbackManager() { return rgbCallbackManager_; }

    // ── Pad logic ───────────────────────────────────────────────────────
    /// Register @p handler under @p appName, replacing any handler already
    /// registered under that name. A null handler is refused.
    void registerPadLogic(const std::string& appName, std::shared_ptr<IPadLogicHandler> handler);
    /// Activate appName's logic. An empty name hands the pads to the base
    /// logic (see setBasePadLogic()); a name that is not registered leaves
    /// no logic active. Blocks, before the old logic's onDeactivate(), until
    /// the pad callbacks other threads are running have returned
    /// (IPadLogicHandler's guarantee); so does every call below that
    /// deactivates a logic.
    void setActivePadLogic(const std::string& appName);
    /// Install the system instrument: the logic that owns the pads whenever no
    /// app has claimed them, so pads keep playing outside any app. Apps preempt
    /// it through pushPadLogic()/setActivePadLogic() and hand it back on
    /// teardown. Pass "" to remove it (pads go silent with no app again).
    ///
    /// Keep the handler registered for as long as it is the base: do not
    /// unregisterPadLogic() a base handler on app teardown.
    void setBasePadLogic(const std::string& appName);
    /// The base logic's name; "" when there is none.
    std::string getBasePadLogic() const;
    /// The active logic's name; "" when no logic is active.
    std::string getActivePadLogic() const;
    /// Remove the handler registered under @p appName, deactivating it first
    /// when it is the active one.
    void unregisterPadLogic(const std::string& appName);
    /// The names of every registered pad logic.
    std::vector<std::string> getRegisteredPadLogics() const;
    /// The handler registered under appName, or null. For a logic that plays
    /// through another -- an app that owns the pads but keeps the base
    /// instrument sounding under them.
    std::shared_ptr<IPadLogicHandler> getPadLogic(const std::string& appName) const;
    /// Save the active pad logic on an internal stack, then activate
    /// appName. Pair with popPadLogic() -- for screens and apps that overlay
    /// a temporary logic and hand control back on teardown.
    ///
    /// Rules:
    /// - call popPadLogic() before unregistering the handler that was active
    ///   at the push; popPadLogic() reactivates it by name;
    /// - do not call it from IPadLogicHandler::onActivate()/onDeactivate(),
    ///   nor from a pad callback (onPadPress() and the rest) -- post the
    ///   change to the UI thread instead (see IPadLogicHandler).
    void pushPadLogic(const std::string& appName);
    /// Re-activate the logic saved by the matching pushPadLogic(). No-op when
    /// the stack is empty. The rules of pushPadLogic() apply.
    void popPadLogic();

    /// The LED controller the manager drives.
    PadLedController& getLedController() { return *ledController_; }
    /// The pad animator the manager ticks.
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
    // Mutable: a const query of the handler (performanceFor) is a dispatch too.
    mutable int dispatchInFlight_ = 0;
    mutable std::condition_variable dispatchIdle_;

    // The active handler for one dispatch, counted in flight while it lives;
    // null (and not counted) when no pad logic is active.
    class DispatchScope {
    public:
        explicit DispatchScope(const PadManager& pm);
        ~DispatchScope();
        DispatchScope(const DispatchScope&) = delete;
        DispatchScope& operator=(const DispatchScope&) = delete;
        std::shared_ptr<IPadLogicHandler> handler;
    private:
        const PadManager& pm_;
    };
    // Under logicMutex_ (released while waiting): returns once no pad-logic
    // callback runs on another thread.
    void waitDispatchIdle(std::unique_lock<std::mutex>& lock);

    // Helpers
    int getPadIndex(uint8_t midiNote);
    void updateLEDState(uint8_t padIdx);
};

/// The platform's PadManager.
PadManager& getPadManager();

} // namespace crosspad
