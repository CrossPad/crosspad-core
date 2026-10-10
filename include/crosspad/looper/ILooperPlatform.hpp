// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/looper/ILooper.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace crosspad {

/// What the looper needs from a platform: memory for the loop, the clock it
/// squares itself with, and somewhere to write an export. Everything else in
/// the engine is portable.
class ILooperPlatform {
public:
    virtual ~ILooperPlatform() = default;

    /// One segment of @p bytes, or nullptr. Asked for repeatedly until the
    /// loop is as long as it was asked to be or memory says no more.
    ///
    /// @p undo marks the copy an overdub keeps so it can be taken back. A
    /// platform may hold a tighter line on the loop itself than on that copy:
    /// the loop is opened the moment the screen is, where the memory is still
    /// everybody's, while the copy is only ever asked for because somebody
    /// pressed overdub.
    virtual void*  allocSegment(size_t bytes, bool undo) = 0;
    virtual void   freeSegment(void* p) = 0;
    /// For "x s is all the memory there is".
    virtual size_t freeMemoryBytes() = 0;
    virtual uint32_t sampleRate() = 0;

    /// Frames in a bar at the tempo the board keeps; 0 when it keeps none, in
    /// which case a loop on the grid is no different from a free one.
    virtual uint32_t framesPerBar() = 0;
    /// Frames until the next bar line, or UINT32_MAX when no clock is
    /// running -- then the footswitch acts at once and the loop is the grid.
    virtual uint32_t framesToBar() = 0;

    /// Take the memory the instruments play from, or give it back, and say
    /// whether it is now held. Whoever answers this stops and starts the
    /// engine that owns it and puts the kit back afterwards -- the engine is
    /// only ever lent with the loop's buffer already released. A platform
    /// with no such engine answers false and the loop stays as it was.
    virtual bool borrowEngine(bool take) { (void)take; return false; }

    /// Start writing the loop as a clip of the open project, laid at the end
    /// of @p track. The platform reads the audio back with
    /// LooperEngine::read() from its own task.
    virtual bool startExport(uint8_t track) = 0;
    virtual LoopExport exportState() = 0;
    virtual std::string exportName() = 0;

    virtual void log(bool warning, const char* msg) = 0;
};

} // namespace crosspad
