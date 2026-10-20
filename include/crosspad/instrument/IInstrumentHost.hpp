// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "crosspad/kit/KitInfo.hpp"

namespace crosspad {

/**
 * @brief Lends the pitched sample engine to an app that plays an instrument
 *        of its own (Pitch Play), and keeps that app's instrument library.
 *
 * One of the two engines is current at a time: the pitched engine or the
 * kits' engine. An app borrows the pitched one: a load stops the kits'
 * engine and builds the app's bank; release() frees the bank and puts the
 * current kit back as it was. Everything that touches the card -- reading WAVs,
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

    /// Where the host is (see State).
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

    /// What the loaded bank made of a zone: the length of its PCM and the
    /// rate it ended up at. A caller setting loop points needs this -- the
    /// spec stores them as frames, and frames mean nothing without a length
    /// to put them against, so a UI without it can only guess.
    ///
    /// Not pure, and zero where nothing is loaded: a host that does not keep
    /// its bank around (the PC simulator) is still a host, and a caller must
    /// treat frames == 0 as "cannot say" rather than "empty".
    struct ZoneAudio {
        uint32_t frames     = 0;
        uint32_t sampleRate = 0;
    };
    virtual ZoneAudio zoneAudio(uint8_t zoneIdx) const { (void)zoneIdx; return {}; }

    /// The file name of an instrument in its folder.
    static constexpr const char* kFileName = "instrument.json";

    /// A zone longer than this, in seconds, is cut to it on load: an
    /// instrument's zones are notes, not whole recordings.
    static constexpr float kMaxZoneSeconds = 12.0f;
};

} // namespace crosspad
