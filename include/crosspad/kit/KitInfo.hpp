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
static constexpr uint8_t kInstrumentMaxZones = 4;

/// One recorded note of a pitched instrument.
struct InstrumentZoneSpec {
    std::string sample;
    float       root      = -1.0f;   ///< < 0 → detect the root pitch at load time
    uint32_t    loopStart = 0;
    uint32_t    loopEnd   = 0;       ///< 0 → one-shot
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
    /// Tempo of the beat this kit ships as preview.mid, in BPM. 0 means the kit
    /// does not say -- and a lot of DAWs export a MIDI file with no tempo map at
    /// all, because they keep tempo in the project, so the file frequently
    /// cannot answer either. Without this the preview falls back to the SMF
    /// default of 120, which is silently wrong for anything else.
    uint16_t bpm = 0;
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
