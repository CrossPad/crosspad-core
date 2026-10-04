#pragma once
#include <cstdint>
#include <ArduinoJson.h>

namespace crosspad {

class AudioSettings {
public:
    struct Delay {
        bool bypass;
        uint8_t length;
        uint8_t feedback;
        uint8_t dry;
        uint8_t wet;
        uint8_t inVolume;
        uint8_t outVolume;

        Delay() : bypass(true), length(50), feedback(50), dry(50), wet(50), inVolume(50), outVolume(50) {}

    };

    struct Reverb {
        bool bypass;
        uint8_t roomSize;
        uint8_t damping;
        uint8_t dry;
        uint8_t wet;
        uint8_t inVolume;
        uint8_t outVolume;

        Reverb() : bypass(true), roomSize(50), damping(50), dry(50), wet(50), inVolume(50), outVolume(50) {}

    };

    struct Distortion {
        bool bypass;
        uint8_t gain;
        uint8_t tone;
        uint8_t level;
        uint8_t inVolume;
        uint8_t outVolume;

        Distortion() : bypass(true), gain(50), tone(50), level(50), inVolume(50), outVolume(50) {}

    };

    struct Chorus {
        bool bypass;
        uint8_t rate;
        uint8_t depth;
        uint8_t feedback;
        uint8_t dry;
        uint8_t wet;
        uint8_t inVolume;
        uint8_t outVolume;

        Chorus() : bypass(true), rate(50), depth(50), feedback(50), dry(50), wet(50), inVolume(50), outVolume(50) {}

    };

    struct Flanger {
        bool bypass;
        uint8_t rate;
        uint8_t depth;
        uint8_t feedback;
        uint8_t dry;
        uint8_t wet;
        uint8_t inVolume;
        uint8_t outVolume;

        Flanger() : bypass(true), rate(50), depth(50), feedback(50), dry(50), wet(50), inVolume(50), outVolume(50) {}

    };

    struct Controller {
        bool bypass;
        uint8_t rate;
        uint8_t depth;
        uint8_t feedback;
        uint8_t output;

        Controller() : bypass(true), rate(50), depth(50), feedback(50), output(50) {}

    };

    struct Eq {
        bool bypass;
        uint8_t low, lowQ;
        uint8_t midLow, midLowQ;
        uint8_t mid, midQ;
        uint8_t midHigh, midHighQ;
        uint8_t high, highQ;
        uint8_t inVolume;
        uint8_t outVolume;

        Eq() : bypass(true),
               low(50), lowQ(50), midLow(50), midLowQ(50),
               mid(50), midQ(50), midHigh(50), midHighQ(50),
               high(50), highQ(50), inVolume(50), outVolume(50) {}

    };

    // --- Main fields ---
    bool mute;
    bool speakersEnabled;
    uint8_t inVolume;
    uint8_t outVolume;
    /* The outputs' volumes, 0..100 (the ES8388's own range -- a UI slider
     * must not go past it).
     *
     * These cells name **what you hear**, not a register:
     *   [0][0] the speaker, [0][1] the headphones, [1][0] the line out.
     *   [1][1] is spare.
     *
     * They used to be read as [codec][OUT1|OUT2] and the two platforms wrote
     * down different wiring for the same board -- one of them a guess made
     * when the only call available set a whole codec at once, which is how
     * the top bar's "Speaker" slider came to drive the headphone amp. Which
     * register each of these reaches is the platform's business now
     * (IdfGuiPlatform), in one place, where it can be corrected by ear
     * without touching anything that reads these. */
    uint8_t outVolumeCodec[2][2];
    uint8_t inVolumeCodec[2][2];
    Delay delay;
    Reverb reverb;
    Distortion distortion;
    Chorus chorus;
    Flanger flanger;

    static constexpr int EQ_MAX_COUNT = 4;
    static constexpr int LFO_MAX_COUNT = 4;
    Eq eq[EQ_MAX_COUNT];
    Controller controler[LFO_MAX_COUNT];

    AudioSettings() : mute(false), speakersEnabled(true), inVolume(50), outVolume(50),
                      outVolumeCodec{}, inVolumeCodec{} {
        outVolumeCodec[0][0] = 64;  // the speaker
        outVolumeCodec[0][1] = 64;  // the headphones
        outVolumeCodec[1][0] = 50;  // the line out
        outVolumeCodec[1][1] = 50;  // spare
    }

    // The JSON form is what CrosspadSettings::saveTo() persists: levels, the
    // codec matrices and each effect's bypass flag. Effect parameters never
    // reach NVS, so a backup carrying them would promise what a reboot forgets.
    void toJson(JsonObject& json) const {
        json["mute"] = mute;
        json["inVolume"] = inVolume;
        json["outVolume"] = outVolume;
        // speakersEnabled and the per-codec matrices are what the hardware
        // actually plays at — without them a settings file round-trips silent.
        json["speakersEnabled"] = speakersEnabled;
        JsonArray outCodec = json["outVolumeCodec"].to<JsonArray>();
        JsonArray inCodec = json["inVolumeCodec"].to<JsonArray>();
        for (int c = 0; c < 2; c++) {
            JsonArray outRow = outCodec.add<JsonArray>();
            JsonArray inRow = inCodec.add<JsonArray>();
            for (int o = 0; o < 2; o++) {
                outRow.add(outVolumeCodec[c][o]);
                inRow.add(inVolumeCodec[c][o]);
            }
        }
        json["delay"]["bypass"] = delay.bypass;
        json["reverb"]["bypass"] = reverb.bypass;
        json["distortion"]["bypass"] = distortion.bypass;
        json["chorus"]["bypass"] = chorus.bypass;
        json["flanger"]["bypass"] = flanger.bypass;
    }

    void fromJson(const JsonObject& json) {
        mute = json["mute"] | mute;
        inVolume = json["inVolume"] | inVolume;
        outVolume = json["outVolume"] | outVolume;
        // Guarded: files written before these keys existed must keep the
        // constructor's volumes rather than being muted to 0.
        speakersEnabled = json["speakersEnabled"] | speakersEnabled;
        JsonVariantConst outCodec = json["outVolumeCodec"];
        JsonVariantConst inCodec = json["inVolumeCodec"];
        for (int c = 0; c < 2; c++) {
            for (int o = 0; o < 2; o++) {
                outVolumeCodec[c][o] = outCodec[c][o] | outVolumeCodec[c][o];
                inVolumeCodec[c][o] = inCodec[c][o] | inVolumeCodec[c][o];
            }
        }
        // Same nesting as the version-1 files, whose effect objects carried
        // the parameters too; only the flag is read.
        delay.bypass = json["delay"]["bypass"] | delay.bypass;
        reverb.bypass = json["reverb"]["bypass"] | reverb.bypass;
        distortion.bypass = json["distortion"]["bypass"] | distortion.bypass;
        chorus.bypass = json["chorus"]["bypass"] | chorus.bypass;
        flanger.bypass = json["flanger"]["bypass"] | flanger.bypass;
    }
};

} // namespace crosspad
