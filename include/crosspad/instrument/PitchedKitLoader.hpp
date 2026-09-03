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
 * @c maxBankBytes caps the resident PCM of one bank; 0 means unlimited.
 */
struct PitchedLoadEnv {
    void* (*alloc)(size_t) = nullptr;
    void  (*free)(void*) = nullptr;
    uint32_t engineRate = 44100;
    std::string (*resolvePath)(const std::string& kitPath, const std::string& sample)
        = &resolveKitSamplePath;
    void (*log)(const char* line) = nullptr;
    size_t maxBankBytes = 0;
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
 * @param fft transform for pitch detection and rate conversion; its
 *        maxSize() decides the resampler's block.
 * @return false on a bad env, an unreadable or unsupported WAV, a bank over
 *         @c maxBankBytes, an allocation failure, or a zone whose root could
 *         neither be read from kit.json nor detected.
 */
bool pitchedBuildBank(const KitInfo& kit, const PitchedLoadEnv& env, IFft& fft,
                      SampleBank*& outBank, char* err, size_t errLen);

} // namespace crosspad
