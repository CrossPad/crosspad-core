// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/sampletools/ISampleTools.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace crosspad {

/// What a platform gives the sample tools engine: a worker thread, memory for
/// its rings and card buffers, the card, and the board's inputs -- which
/// codec's ADC a take may hold, and whether USB audio is there to record.
/// Everything else in the engine is portable.
class ISampleToolsPlatform {
public:
    virtual ~ISampleToolsPlatform() = default;

    /// Started on the first request that needs it, never more than once.
    virtual bool startWorker(void (*entry)(void*), void* arg) = 0;
    /// After the worker was told to quit. A platform whose engine lives as
    /// long as it runs may do nothing.
    virtual void joinWorker() = 0;
    virtual void wake() = 0;
    /// Returns when woken or after @p timeoutMs; UINT32_MAX waits for a wake.
    virtual void waitWake(uint32_t timeoutMs) = 0;
    /// 0: the shortest sleep the platform has (a scheduler tick).
    virtual void sleepMs(uint32_t ms) = 0;
    virtual int64_t nowUs() = 0;

    /// One ring segment of @p bytes, or nullptr. @p missingSegs is how many
    /// the ring still needs to reach its minimum, for a platform that keeps a
    /// reserve for its UI. With @p mayBorrow the platform may lend memory it
    /// will want back (@p lent true): the engine then gives such a ring up
    /// when asked (SampleToolsEngine::reclaimLentRing()).
    virtual void* allocRingSegment(size_t bytes, uint32_t missingSegs, bool mayBorrow, bool& lent) = 0;
    /// Any segment allocRingSegment() gave, lent or not.
    virtual void  freeRingSegment(void* p) = 0;
    /// The worker's card buffers, allocated once.
    virtual void* allocScratch(size_t bytes) = 0;

    /// The card can be opened right now.
    virtual bool storageReady() = 0;
    /// A bulk read or write gives way to whatever else must read the card in
    /// time (the board's sample streamer).
    virtual void yieldIo() = 0;
    /// The card's top: takes go under <root>/crosspad/recordings, kits under
    /// <root>/crosspad/kits -- the kit manager's root, whose logical paths
    /// start at "/crosspad".
    virtual std::string cardRoot() = 0;

    /// Hold codec @p codec's ADC on its own input (0 the mics, 1 the line-in)
    /// while a take listens to it, or let it go. False when the platform has
    /// nothing to hold: the input is then recorded as it is routed.
    virtual bool claimCodec(uint8_t codec, bool on) { (void)codec; (void)on; return false; }
    /// Whether @p in can be recorded right now, for the inputs the engine
    /// cannot tell by itself (it knows Pitch): USB only while the board is in
    /// its USB audio profile.
    virtual bool inputAvailable(RecInput in) { return in != RecInput::Usb; }
    /// Render: whatever else the platform plays on the "Preview" channel (an
    /// audition of the board's own sampler), added into @p out first.
    virtual void mixAudition(float* out, uint32_t frames) { (void)out; (void)frames; }

    virtual void log(bool warning, const char* msg) = 0;
};

} // namespace crosspad
