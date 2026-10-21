// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SamplerKit.hpp
 * @brief A kit as the engine plays it: sixteen pads, their samples and the
 *        way each one is meant to sound.
 *
 * This is deliberately not `crosspad::Kit`. That one is the editor's model --
 * colours, names, dirty flags, the JSON round-trip. This is what the render
 * thread reads, so it holds only what makes sound, in the form the voice
 * wants it: slot indices rather than paths, a phase increment rather than a
 * semitone count, linear gains rather than 0..127.
 *
 * Built by the loader, published to the audio thread as a whole. Never edited
 * in place while it is published -- the engine's mutation window is for that.
 *
 * Three fields of the kit JSON are honoured here that the engine this replaces
 * could not honour at all, and which the firmware therefore realised by
 * writing transposed copies of the files to the card (`requestRendered`):
 * `pitch`, and the per-pad rate of a file that is not at the engine's rate.
 * `hp`, `lp` and `reverse` stay pre-rendered for now; they are filters and a
 * direction, not a step, and they belong in a later pass.
 */

#include <cstdint>

namespace crosspad {

/// What a pad does with a release, straight from the kit's `playbackMode`.
enum class PadMode : uint8_t {
    OneShot = 0,   ///< plays to its end; a release is ignored
    Hold    = 1,   ///< a release stops it
    Loop    = 2,   ///< repeats until something stops it
};

/// One layer of one pad.
struct KitLayer {
    uint16_t slot  = 0xFFFF;   ///< SampleStore slot
    uint32_t start = 0;        ///< trim, in the source file's frames
    uint32_t end   = 0;        ///< exclusive; 0 means "to the end of the file"
};

struct KitPad {
    static constexpr uint8_t kMaxLayers = 8;

    KitLayer layers[kMaxLayers];
    uint8_t  layerCount = 0;

    PadMode  mode  = PadMode::OneShot;
    uint8_t  choke = 0;        ///< 0 = no group; 1..7 cut each other

    /**
     * @brief Which layer a hit plays.
     *
     * The engine being replaced decided this inside a binary, and the only
     * way to see what it chose was its NoteOn callback. Here it is written
     * down: layers are a **velocity split**, low to high, in the order the kit
     * lists them -- layer i covers velocities [i * 128 / n, (i+1) * 128 / n).
     * A one-layer pad, which is every pad the editor can currently make, is
     * that layer at every velocity.
     *
     * `cycle` in the kit JSON meant round-robin and was read by nothing for as
     * long as it has existed. It is honoured here: with it set, successive
     * hits take successive layers instead of splitting by velocity.
     */
    bool roundRobin = false;

    float gain  = 1.0f;        ///< from volume 0..127
    float pan   = 0.0f;        ///< -1 left .. +1 right, from pan 0..127
    int8_t semitones = 0;      ///< realtime, from `pitch`
    float  attackMs = -1.0f;   ///< the hit's fade-in; < 0 = SamplerEngine::Params::attackMs
};

struct SamplerKit {
    static constexpr uint8_t kPads = 16;
    KitPad pad[kPads];

    /// 0..127 to a linear gain, as the mixer's faders do.
    static float volumeToGain(int volume)
    {
        if (volume <= 0) return 0.0f;
        const float v = (volume > 127 ? 127 : volume) / 100.0f;   // 100 is unity in the kit JSON
        return v;
    }

    /// The kit's 0..127 pan, 64 centre, to -1..+1.
    static float panToBalance(int pan)
    {
        if (pan < 0) pan = 0;
        if (pan > 127) pan = 127;
        return (pan - 64) / 63.0f;
    }

    /// Balance, as the mixer: unity at the centre, a side only taken down.
    static void panGains(float balance, float& l, float& r);
};

} // namespace crosspad
