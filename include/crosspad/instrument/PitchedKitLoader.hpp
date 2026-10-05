// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file PitchedKitLoader.hpp
 * @brief KitInfo -> SampleBank: the whole load of a pitched kit, portably.
 *
 * Read each zone's WAV, convert it to the engine's rate when it does not
 * already match, find the root note when kit.json did not say, and hand back
 * a bank the caller owns. Nothing here knows what a filesystem mount or a
 * PSRAM heap is: memory, path resolution and logging arrive as callbacks in
 * PitchedLoadEnv, which is the only thing a platform writes.
 *
 * Load-time code. It reads files and allocates megabytes — never call it from
 * an audio thread, and never while the bank it is replacing is being
 * rendered (PitchedInstrument::installBank() is the swap that makes that
 * safe).
 */

#include "crosspad/dsp/IFft.hpp"
#include "crosspad/instrument/SampleBank.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/KitPathUtils.hpp"

#include <cstddef>
#include <string>

namespace crosspad {

/**
 * @brief Everything the loader needs from its platform.
 *
 * @c alloc / @c free own the zone PCM *and* the transient buffers a rate
 * conversion needs (source copy plus overlap-add scratch, which together can
 * exceed the zone itself). On the device both are PSRAM.
 * @c resolvePath turns kit.json's logical sample name into something stdio
 * can open — the mount prefix is a platform fact and is applied here. Null
 * falls back to crosspad::resolveKitSamplePath(), which is right wherever the
 * logical path is already the real one (PC, tests) and wherever
 * setKitPathFsPrefix() has been told the mount.
 * @c log receives one already-formatted line; null discards.
 * @c maxBankBytes caps the resident PCM of one bank; 0 means unlimited. A zone
 * that does not fit what is left of it is **truncated to what fits**, not
 * refused: the arena holds about a minute of mono 16-bit at 44.1 kHz, and a
 * kit built around a longer take should play its first minute rather than
 * fail to load. Only a zone with no room left at all is an error.
 */
struct PitchedLoadEnv {
    void* (*alloc)(size_t) = nullptr;
    void  (*free)(void*) = nullptr;
    uint32_t engineRate = 44100;
    std::string (*resolvePath)(const std::string& kitPath, const std::string& sample)
        = &resolveKitSamplePath;
    void (*log)(const char* line) = nullptr;
    size_t maxBankBytes = 0;
    /// Longest a zone may be, in engine-rate frames; 0 leaves the budget the
    /// only limit. An instrument is notes, not takes: a minute picked by
    /// mistake would otherwise convert for a minute and fill the bank.
    uint32_t maxZoneFrames = 0;
    /// Called between blocks of reading and converting, so the thread that
    /// builds a bank can let others run: converting a few seconds of 44.1 kHz
    /// to 48 kHz is seconds of CPU. Null never yields.
    void (*yield)() = nullptr;
    /// Convert a zone at another rate than the engine's when it is loaded --
    /// band-limited, and seconds of CPU per second of audio on a device -- or
    /// keep it at its own rate and let the voice step by the ratio as it
    /// plays (Zone::rate): free, with the varispeed's own interpolation, which
    /// every transposed note goes through anyway.
    bool convertRate = true;
    /// Give each zone the gain that brings its peak to kNormalizePeak (at
    /// most kNormalizeMaxGain, so a zone of near-silence is not raised into
    /// noise). Off, a zone plays at the level it was recorded.
    bool normalize = false;
};

/// Where PitchedLoadEnv::normalize puts a zone's peak: -1 dBFS.
constexpr float kNormalizePeak = 0.89f;
/// The most PitchedLoadEnv::normalize raises a zone: +24 dB.
constexpr float kNormalizeMaxGain = 16.0f;

/**
 * @brief What a load did beyond succeeding, for the caller to show.
 *
 * @c warning names the first truncated zone and is empty when nothing was
 * cut; @c truncatedZones and @c droppedSeconds cover all of them.
 */
struct PitchedLoadReport {
    uint8_t zones = 0;
    uint32_t truncatedZones = 0;
    float droppedSeconds = 0;
    char warning[96] = {};
};

/**
 * @brief Build the sample bank for @p kit.
 *
 * On success @p outBank holds a bank the caller owns and must eventually
 * `delete` — after the render gate proves the audio thread is out of it.
 * On failure nothing is allocated: a partially built bank is destroyed before
 * returning, @p outBank is left alone and @p err carries a one-line reason
 * naming the zone at fault.
 *
 * Each zone is read, converted and written straight into its final buffer a
 * block at a time, so peak memory is the bank plus one resampler block --
 * loading a zone never costs a second copy of it.
 *
 * @param fft transform for pitch detection and rate conversion; its
 *        maxSize() decides the resampler's block.
 * @param report filled in when non-null: zone count, and what @c maxBankBytes
 *        cut short. A truncated zone is a warning, not a failure.
 * @return false on a bad env, an unreadable or unsupported WAV, a zone with no
 *         budget left at all, an allocation failure, or a zone whose root
 *         could neither be read from kit.json nor detected.
 */
bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen,
                      PitchedLoadReport* report);

/// The same load, for a caller with nothing to say about truncation.
bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen);

} // namespace crosspad
