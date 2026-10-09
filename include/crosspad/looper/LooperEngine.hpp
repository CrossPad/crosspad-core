// SPDX-License-Identifier: MIT
#pragma once

#include "crosspad/looper/ILooper.hpp"
#include "crosspad/looper/ILooperPlatform.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace crosspad {

/// One track of looping, portable: the loop itself, the state machine a pedal
/// has, overdub, and one level of undo.
///
/// **Mono, 16-bit.** A loop is the one thing on this board that must be held
/// whole in memory, and there is about a megabyte to be had beside the sample
/// engine -- which is twelve seconds in mono and six in stereo. Mono is also
/// what a microphone or a jack gives, and the loop is played to both sides.
///
/// **Segmented.** The buffer is 16 kB pieces, not one block: after a few apps
/// have come and gone the largest free piece of PSRAM is well under 100 kB,
/// and a loop that needed one contiguous megabyte would simply never start.
///
/// **Who touches what.** The audio thread owns the position and applies every
/// transition at a block boundary (rtInput() then rtRender(), in that order,
/// once a block); an app only ever posts a request. The one thing done on the
/// app's thread is the copy an overdub needs for undo, and that is made while
/// the state is still Play -- so the audio thread is reading the buffer, not
/// writing it, and the copy is of what the loop was.
class LooperEngine final : public ILooper {
public:
    static constexpr uint32_t kSegFrames = 8192;        ///< 16 kB of int16
    static constexpr uint32_t kMaxSegs   = 192;         ///< ~32 s at 48 kHz
    static constexpr uint8_t  kMaxLayers = 99;

    explicit LooperEngine(ILooperPlatform& platform);
    ~LooperEngine() override;

    // ── ILooper ──────────────────────────────────────────────────────────
    bool open(float seconds) override;
    void close() override;
    LoopStatus status() const override;
    void footswitch() override;
    void stop() override;
    void play() override;
    void undo() override;
    void clear() override;
    void setGrid(LoopGrid grid) override;
    bool setMemory(LoopMemory memory) override;

    /// Ask the memory again for what it would not give yet, and grow the
    /// buffer into it. A loop opened while the arena was somebody else's --
    /// a kit on its way back, say -- comes out a second long, and nothing has
    /// to be lost to put that right: the buffer only ever grows, and never
    /// while a pass is being written. True when it did grow.
    bool topUp();
    void setLevel(float gain) override;
    float level() const override;
    void envelope(uint8_t* out, uint32_t n) const override;
    bool requestExport(uint8_t track) override;
    LoopExport exportState() const override;
    std::string exportName() const override;

    // ── the audio thread ─────────────────────────────────────────────────
    /// The selected input, interleaved stereo, before the loop is rendered.
    void rtInput(const int16_t* stereo, uint32_t frames);
    /// Add the loop to @p out (interleaved stereo) and move the position on.
    void rtRender(float* out, uint32_t frames);

    /// The memory is being taken back by whatever lends it: the loop goes and
    /// the engine is Shared again. The platform calls this instead of asking
    /// politely -- a kit being loaded needs its engine now, and it is already
    /// in the middle of taking it, so nothing here may call back into it.
    void surrenderEngine();

    /// The loop's audio, mono, for whoever writes it to a file. Not for the
    /// audio thread; safe while the loop plays.
    uint32_t read(uint32_t from, int16_t* dst, uint32_t n) const;

private:
    /// What the next block is to do, posted by an app.
    enum class Req : uint8_t { None, Foot, Stop, Play, Clear };

    int16_t* at(std::vector<int16_t*>& segs, uint32_t frame) const
    { return segs[frame / kSegFrames] + frame % kSegFrames; }
    const int16_t* at(const std::vector<int16_t*>& segs, uint32_t frame) const
    { return segs[frame / kSegFrames] + frame % kSegFrames; }

    void release(std::vector<int16_t*>& segs);
    bool grow(std::vector<int16_t*>& segs, uint32_t frames, bool undo);
    /// The audio thread: carry out whatever was posted.
    void apply(Req r);
    /// An app's thread: the loop as it stands, kept so undo can put it back.
    void prepareUndo();
    /// Close the first pass: the length is what was played, or whole bars.
    void closeLoop();

    ILooperPlatform& p;

    /* Everything that takes or gives back memory, and nothing the audio
     * thread does. There are three callers and they are three threads: the
     * screen's timer, a host over the wire, and whatever is starting the
     * sample engine and wants its arena back -- and two of them changing the
     * segment list at once is the segment list in pieces. Recursive because
     * setMemory() is close() and open(). */
    mutable std::recursive_mutex memMutex_;

    std::vector<int16_t*> buf_;      ///< the loop
    std::vector<int16_t*> undo_;     ///< what it was before this overdub pass
    uint32_t capacity_ = 0;          ///< frames the buffer holds
    uint32_t rate_ = 0;

    std::atomic<LoopState> state_{LoopState::Empty};
    std::atomic<LoopGrid>  grid_{LoopGrid::Free};
    std::atomic<LoopMemory> memory_{LoopMemory::Shared};
    float                  asked_ = 0.0f;      ///< seconds open() was asked for
    std::atomic<uint32_t>  frames_{0};      ///< the loop's length
    /* Frames that have really been written since the loop was cleared. A
     * length rounded up to the bar reaches past it, and what is past it is
     * whatever the memory held -- so it is read as silence and no megabyte
     * ever has to be zeroed on the audio thread. An overdub pass moves it
     * forward as it fills the rest in. */
    std::atomic<uint32_t>  recorded_{0};
    std::atomic<uint32_t>  pos_{0};
    std::atomic<uint32_t>  waiting_{0};     ///< frames until a posted foot acts
    std::atomic<Req>       req_{Req::None};
    std::atomic<Req>       armed_{Req::None};   ///< posted, waiting for the line
    std::atomic<uint8_t>   layers_{0};
    std::atomic<bool>      undoReady_{false};   ///< undo_ holds the pass before
    std::atomic<uint32_t>  revision_{0};
    std::atomic<float>     gain_{1.0f};
    std::atomic<int32_t>   pkIn_{0}, pkLoop_{0};
    /// Set while an app is copying the loop for undo: the audio thread must
    /// not start summing into it until the copy is out of the way.
    std::atomic<bool>      copying_{false};
};

} // namespace crosspad
