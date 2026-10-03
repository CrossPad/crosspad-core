// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace crosspad {

/// Where the looper's one track is.
enum class LoopState : uint8_t {
    Empty,     ///< nothing in it
    Rec,       ///< the first pass, which is what sets the length
    Play,
    Overdub,   ///< playing, and summing the input into the loop as it passes
    Stopped,   ///< a loop is held, silent
};

/// What the length of a loop is: exactly what was played, or whole bars at
/// the tempo the board is keeping -- so a loop and the sequencer stay in step.
enum class LoopGrid : uint8_t { Free, Sync };

/// Where an export is.
enum class LoopExport : uint8_t { Idle, Busy, Done, Failed };

struct LoopStatus {
    LoopState state = LoopState::Empty;
    LoopGrid  grid  = LoopGrid::Free;
    uint32_t  frames   = 0;        ///< the loop's length
    uint32_t  position = 0;        ///< where it is playing
    uint32_t  capacity = 0;        ///< the longest loop the memory holds
    uint32_t  sampleRate = 0;
    uint16_t  bars   = 0;          ///< its length in bars; 0 when off the grid
    uint8_t   layers = 0;          ///< passes in it: 1 is the first take
    bool      canUndo = false;
    bool      waiting = false;     ///< the footswitch is waiting for a bar line
    uint32_t  revision = 0;        ///< moves whenever the loop's audio changed
    float     peakIn   = 0.0f;     ///< the input, for the meter
    float     peakLoop = 0.0f;
};

/// One track of looping: record a pass, overdub onto it, undo the last one,
/// and play it out. The engine is portable (crosspad/looper/LooperEngine) --
/// this is what an app sees, because the memory a loop lives in, the clock it
/// can be squared with, and the file an export writes are the platform's.
///
/// Nothing here blocks: the transport calls are requests the audio thread
/// applies on its next block (and, on the grid, on the next bar line), and an
/// export is a job whose progress is polled.
class ILooper {
public:
    virtual ~ILooper() = default;

    /// Memory for a loop of up to @p seconds -- shorter when that is all
    /// there is, and false when there is not even a second of it. Calling it
    /// again with a loop already recorded does nothing.
    virtual bool open(float seconds) = 0;
    /// Give the memory back; the loop goes with it.
    virtual void close() = 0;
    virtual LoopStatus status() const = 0;

    /// One button, the way a pedal has one: empty records, recording closes
    /// the loop and plays it, playing overdubs, overdubbing goes back to
    /// playing. On the grid, with a clock running, it takes effect on the
    /// next bar line -- status().waiting says so until it does.
    virtual void footswitch() = 0;
    virtual void stop() = 0;
    virtual void play() = 0;
    /// Drop the last overdub pass; only until the next one starts.
    virtual void undo() = 0;
    virtual void clear() = 0;
    virtual void setGrid(LoopGrid grid) = 0;
    virtual void setLevel(float gain) = 0;
    virtual float level() const = 0;

    /// A drawing of the loop: @p n buckets, the loudest sample of each as
    /// 0..255. Walks the loop, so ask when status().revision moves.
    virtual void envelope(uint8_t* out, uint32_t n) const = 0;

    /// Write the loop into the open song project as a clip and lay it at the
    /// end of @p track, where Arrange picks it up. Poll exportState().
    virtual bool requestExport(uint8_t track) = 0;
    virtual LoopExport exportState() const = 0;
    /// The clip the last export wrote, or why it failed.
    virtual std::string exportName() const = 0;
};

} // namespace crosspad
