// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/song/AudioSong.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace crosspad {

/// What a platform gives the song engine: a worker thread, memory for its
/// rings, where projects live, and control of the bus a bounce records.
/// Everything else in the engine is portable.
class ISongEnginePlatform {
public:
    virtual ~ISongEnginePlatform() = default;

    virtual bool startWorker(void (*entry)(void*), void* arg) = 0;
    /// After the worker was told to quit. A platform whose engine lives as
    /// long as it runs may do nothing.
    virtual void joinWorker() = 0;
    virtual void wake() = 0;
    /// Returns when woken or after @p timeoutMs; UINT32_MAX waits for a wake.
    virtual void waitWake(uint32_t timeoutMs) = 0;
    virtual void sleepMs(uint32_t ms) = 0;
    virtual int64_t nowUs() = 0;

    /// One ring segment of @p bytes, or nullptr. @p missingSegs is how many
    /// the ring still needs to reach its minimum, for a platform that keeps a
    /// reserve for its UI.
    virtual void* allocRingSegment(size_t bytes, uint32_t missingSegs) = 0;
    virtual void  freeRingSegment(void* p) = 0;
    /// The worker's card buffer and mix-down sum, allocated once.
    virtual void* allocScratch(size_t bytes) = 0;
    /// For "not enough memory (N kB free)".
    virtual size_t freeMemoryBytes() = 0;

    /// The storage the projects are on can be opened right now.
    virtual bool storageReady() = 0;
    /// A bulk read or write gives way to whatever else must read the storage
    /// in time (the board's sample streamer).
    virtual void yieldIo() = 0;
    virtual const char* songsRoot() = 0;
    /// The project last open; empty when none was.
    virtual std::string loadCurrent() = 0;
    virtual void saveCurrent(const std::string& name) = 0;
    /// Live's empty Set a song export is built from: crosspad-core's
    /// data/live12_set.xml.gz, uncompressed (~110 kB, held for the export).
    /// Empty when there is no memory for it, or no way to inflate it.
    virtual std::string liveSetTemplate() { return {}; }

    /// The pads-only bus at unity and unmuted for a bounce; the end puts back
    /// what the player had.
    virtual void beginPadsCapture() = 0;
    virtual void endPadsCapture() = 0;

    /// The seq track's fader on the engine its notes play through -- @p
    /// source's: the kit's engines, or the pitched instrument -- while the
    /// song plays, on top of what the Mixer has for it; 1 when it stops, so
    /// a bounce, which stops it first, records at the Mixer's level. Called
    /// only when it changes. A platform without a mixer leaves it alone.
    virtual void setSeqGain(SeqSource source, float gain) { (void)source; (void)gain; }

    /// A song has opened (or was made): @p song is it, read under the song's
    /// lock on the engine's worker -- copy what is needed and return. The
    /// platform loads the sounds it names (AudioSong::kit, ::instrument).
    virtual void songOpened(const AudioSong& song) { (void)song; }

    virtual void log(bool warning, const char* msg) = 0;
};

} // namespace crosspad
