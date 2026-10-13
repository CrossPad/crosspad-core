// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <crosspad/pad/Pad.hpp>

#ifndef KIT_PADS
#define KIT_PADS 16
#endif

namespace crosspad {

/// Which sample engine plays a kit. Absent/unknown "engine" in kit.json = Sampler.
enum class KitEngine : uint8_t {
    Sampler = 0,
    Pitched = 1
};

/// Mirrors SampleBank::kMaxZones — zones beyond this are dropped at parse time.
static constexpr uint8_t kInstrumentMaxZones = 24;

/// One recorded note of a pitched instrument.
struct InstrumentZoneSpec {
    std::string sample;
    float       root      = -1.0f;   ///< < 0 → detect the root pitch at load time
    /// Where a note begins -- the attack -- before it reaches the loop.
    uint32_t    start     = 0;
    uint32_t    loopStart = 0;
    uint32_t    loopEnd   = 0;       ///< 0 → one-shot
    /// Frames before loopEnd crossfaded into the frames before loopStart, so
    /// the wrap has no seam. 0 = a plain loop. See Zone::loopXfade.
    uint32_t    loopXfade = 0;
};

/// The "instrument" block of a pitched kit.json.
struct InstrumentSpec {
    std::vector<InstrumentZoneSpec> zones;
    uint8_t  padNotes[KIT_PADS] = {};
    bool     padNotesSet        = false;
    uint8_t  baseNote           = 60;
    uint16_t releaseMs          = 150;
    uint16_t attackMs           = 2;
    uint8_t  maxVoices          = 24;

    /**
     * @brief Instrument edits made since the kit was read, not yet written.
     *
     * The counterpart of Pad::dirtyModified for the pitched engine: the editor
     * sets it when a root or a loop point moves, the save path clears it. It
     * describes the session, not the kit, so nothing serialises it.
     */
    bool     dirty              = false;

    /// Explicit pad_notes when given, otherwise a chromatic run from base_note.
    uint8_t noteForPad(uint8_t pad) const {
        if (pad >= KIT_PADS) return baseNote;
        if (padNotesSet)     return padNotes[pad];
        int note = static_cast<int>(baseNote) + static_cast<int>(pad);
        if (note < 0)   note = 0;
        if (note > 127) note = 127;
        return static_cast<uint8_t>(note);
    }
};

struct KitInfo {
    enum Type : uint8_t {
        Normal  = 0,
        NoAudio = 1,
        Custom  = 2
    };

    std::string path;
    std::string name        = "Default Kit";
    std::string author      = "CrossPad";
    std::string description;
    std::string version;
    std::string created_at;
    /// Where the kit comes from, as the kit browser files it: "" for the kits
    /// a board ships with (their artwork beside them), "user" for one made on
    /// it -- the Slicer writes that into every kit it saves.
    std::string category;
    /// A kit of the user's own: said so, or written by the Slicer before it
    /// said so.
    bool isUser() const { return category == "user" || author == "CrossPad Slicer"; }
    /// The kit's tempo in BPM: what the click counts and the loop recorder is
    /// quantised to once the kit is loaded, and what preview.mid is played at.
    /// 0 means the kit does not say -- and a lot of DAWs export a MIDI file
    /// with no tempo map at all, because they keep tempo in the project, so the
    /// file frequently cannot answer either. Without this the preview falls
    /// back to the SMF default of 120, which is silently wrong for anything
    /// else, and the sequencer keeps whatever the previous kit was counting.
    uint16_t bpm = 0;
    /// The kit is played at its samples' own rate on purpose: nobody is
    /// offered to convert it to the engine's (JSON "keep_rate", see
    /// KitRateConvert.hpp). Off unless the kit says so.
    bool keepRate = false;
    Type        type          = Normal;
    int         total_samples = 0;
    bool        parsed        = false;

    KitEngine      engine = KitEngine::Sampler;
    InstrumentSpec instrument;

    Pad pads[KIT_PADS];

    KitInfo() = default;
    explicit KitInfo(const std::string& kitPath) : path(kitPath) {}

    void createDefaultSamples() {
        for (uint8_t n = 0; n < KIT_PADS; n++) {
            pads[n].AddDefaultSample();
        }
    }

    const char* engineName() const {
        return engine == KitEngine::Pitched ? "pitched" : "sampler";
    }

    std::string getDetails() const {
        return name + " by " + author + ", " + description + ", " + version
             + ", " + created_at + ", " + std::to_string(total_samples)
             + " samples, " + (parsed ? "parsed" : "not parsed")
             + ", " + engineName();
    }
};

} // namespace crosspad
