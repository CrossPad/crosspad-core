#pragma once

#include <cstdint>
#include <string>

#include "crosspad/kit/KitInfo.hpp"

namespace crosspad {

/**
 * @brief Lends the pitched sample engine to an app that plays an instrument
 *        of its own (Pitch Play), and keeps that app's instrument library.
 *
 * The pitched engine and the kits' streamer share one memory arena, so only
 * one of them is current. An app borrows it: a load stops the kits' engine
 * and builds the app's bank; release() frees the bank and puts the current
 * kit back as it was. Everything that touches the card -- reading WAVs,
 * reading and writing instrument files, copying samples -- runs on the host's
 * worker, never on the caller's (UI) thread: a request returns at once and
 * the caller polls state().
 *
 * An instrument is a kit.json with "engine":"pitched" (KitInfo::instrument),
 * kept in libraryRoot() rather than among the kits, so the Kits list never
 * shows it. Zone samples resolve the way a kit's do: relative to the folder
 * of KitInfo::path, or as absolute logical paths ("/crosspad/...").
 */
class IInstrumentHost {
public:
    enum class State : uint8_t {
        Idle,     ///< the kits have the engine
        Busy,     ///< a request is running
        Ready,    ///< the app's bank is playing
        Failed,   ///< the last request failed; message() says why
    };

    virtual ~IInstrumentHost() = default;

    /// Take the engine, if it is not held already, and build @p spec's bank.
    /// A request made while another runs waits for it; one already waiting is
    /// replaced (newest wins). State stays Busy until the last is through.
    /// False only where the host cannot take requests at all.
    virtual bool requestLoad(const KitInfo& spec) = 0;

    /// Read the instrument file at @p path (logical, like KitInfo::path) and
    /// load it. instrument() is what was read once it is Ready.
    virtual bool requestOpen(const std::string& path) = 0;

    /// Write @p spec to its KitInfo::path, making the folder. A zone whose
    /// sample lives outside that folder is copied into it first, so an
    /// instrument outlives the takes it was made from. On success
    /// instrument() is the spec as written (its zones relative).
    virtual bool requestSave(const KitInfo& spec) = 0;

    /// Give the engine back: the bank goes and the current kit is reloaded.
    /// Safe when nothing is held. Call from the UI thread.
    virtual void release() = 0;

    virtual State state() const = 0;

    /// The instrument of the last load, open or save that succeeded, its
    /// zones' roots filled in where the load detected them.
    virtual KitInfo instrument() const = 0;

    /// Why the last request failed, or what a load that succeeded had to cut
    /// short. Empty when there is nothing to say.
    virtual std::string message() const = 0;

    /// The folder instruments live in, logical ("/crosspad/pitchplay"): one
    /// sub-folder per instrument, its file named kFileName.
    virtual std::string libraryRoot() const = 0;

    static constexpr const char* kFileName = "instrument.json";

    /// A zone longer than this is cut to it on load: an instrument is notes,
    /// and a Recorder take minutes long picked by mistake would otherwise
    /// convert for minutes and fill the bank.
    static constexpr float kMaxZoneSeconds = 12.0f;
};

} // namespace crosspad
