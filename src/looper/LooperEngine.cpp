// SPDX-License-Identifier: MIT
#include "crosspad/looper/LooperEngine.hpp"

#include <algorithm>
#include <cstring>

namespace crosspad {

namespace {
constexpr uint32_t kSegBytes = LooperEngine::kSegFrames * sizeof(int16_t);
inline int16_t clip16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
/// Interleaved stereo to one channel: a loop is mono, and an input that is
/// the same on both sides (a jack, the pads bus) must not come out 6 dB down.
inline int32_t mono_of(const int16_t* f)
{
    return ((int32_t)f[0] + (int32_t)f[1]) / 2;
}
} // namespace

LooperEngine::LooperEngine(ILooperPlatform& platform) : p(platform) {}

LooperEngine::~LooperEngine()
{
    release(buf_);
    release(undo_);
}

void LooperEngine::release(std::vector<int16_t*>& segs)
{
    for (int16_t* s : segs) p.freeSegment(s);
    segs.clear();
}

bool LooperEngine::grow(std::vector<int16_t*>& segs, uint32_t frames, bool undo)
{
    const uint32_t want = std::min(kMaxSegs, (frames + kSegFrames - 1) / kSegFrames);
    while ((uint32_t)segs.size() < want) {
        void* s = p.allocSegment(kSegBytes, undo);
        if (!s) break;
        segs.push_back(static_cast<int16_t*>(s));
    }
    /* The undo copy is all or nothing: it is walked to the loop's length, so
     * a copy that came out short would be read past its end -- which is
     * exactly what it did, into the heap's canary, until this line said so.
     * The loop itself may come out shorter than it was asked for; it is only
     * no use at all under a second. */
    if (undo) return (uint32_t)segs.size() >= want;
    return (uint32_t)segs.size() * kSegFrames >= (rate_ ? rate_ : 44100u);
}

bool LooperEngine::open(float seconds)
{
    if (capacity_) return true;
    rate_ = p.sampleRate() ? p.sampleRate() : 44100;
    if (seconds < 1.0f) seconds = 1.0f;
    if (!grow(buf_, (uint32_t)(seconds * (float)rate_), false)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "looper: no memory for a loop (%u kB free)",
                 (unsigned)(p.freeMemoryBytes() / 1024));
        p.log(true, msg);
        release(buf_);
        return false;
    }
    capacity_ = (uint32_t)buf_.size() * kSegFrames;
    state_.store(LoopState::Empty);
    frames_.store(0);
    recorded_.store(0);
    pos_.store(0);
    layers_.store(0);
    undoReady_.store(false);
    revision_.fetch_add(1);
    return true;
}

void LooperEngine::close()
{
    /* The audio thread must be out of the buffer before it goes. It takes the
     * latch on the next block and never touches the loop again. */
    copying_.store(true, std::memory_order_release);
    state_.store(LoopState::Empty);
    frames_.store(0);
    recorded_.store(0);
    pos_.store(0);
    layers_.store(0);
    undoReady_.store(false);
    release(buf_);
    release(undo_);
    capacity_ = 0;
    revision_.fetch_add(1);
    copying_.store(false, std::memory_order_release);
}

LoopStatus LooperEngine::status() const
{
    LoopStatus s;
    s.state      = state_.load();
    s.grid       = grid_.load();
    s.frames     = frames_.load();
    s.position   = pos_.load();
    s.capacity   = capacity_;
    s.sampleRate = rate_;
    s.layers     = layers_.load();
    s.canUndo    = undoReady_.load();
    s.waiting    = waiting_.load() != 0;
    s.revision   = revision_.load();
    s.peakIn     = (float)const_cast<std::atomic<int32_t>&>(pkIn_).exchange(0) / 32768.0f;
    s.peakLoop   = (float)const_cast<std::atomic<int32_t>&>(pkLoop_).exchange(0) / 32768.0f;
    if (s.grid == LoopGrid::Sync && s.frames) {
        const uint32_t fpb = const_cast<ILooperPlatform&>(p).framesPerBar();
        if (fpb) s.bars = (uint16_t)((s.frames + fpb / 2) / fpb);
    }
    return s;
}

void LooperEngine::prepareUndo()
{
    const uint32_t len = frames_.load();
    if (!len) return;
    undoReady_.store(false);
    if (!grow(undo_, len, true)) {
        /* Not enough left for a second copy: overdubbing still works, there
         * is just nothing to go back to, and the screen says so. */
        release(undo_);
        p.log(false, "looper: no memory for undo; overdub will not be undoable");
        return;
    }
    /* Copied while the state is still Play, so the audio thread is reading
     * the loop and not writing it. Never past what either side holds. */
    const uint32_t have = std::min((uint32_t)undo_.size(), (uint32_t)buf_.size()) * kSegFrames;
    const uint32_t copy = std::min(len, have);
    for (uint32_t f = 0; f < copy; f += kSegFrames) {
        const uint32_t n = std::min(kSegFrames, copy - f);
        std::memcpy(at(undo_, f), at(buf_, f), n * sizeof(int16_t));
    }
    if (copy < len) { release(undo_); return; }
    undoReady_.store(true);
}

void LooperEngine::footswitch()
{
    if (!capacity_) return;
    /* Pressed twice before the line it was waiting for: the second press
     * takes the first one back. */
    if (waiting_.load()) {
        waiting_.store(0);
        armed_.store(Req::None);
        return;
    }
    /* The copy undo needs is made here, before the pass starts: on the app's
     * thread, with the loop still only being read. */
    if (state_.load() == LoopState::Play && frames_.load()) prepareUndo();
    if (grid_.load() == LoopGrid::Sync) {
        const uint32_t toBar = p.framesToBar();
        if (toBar != UINT32_MAX && toBar > 0) {
            armed_.store(Req::Foot);
            waiting_.store(toBar);
            return;
        }
    }
    req_.store(Req::Foot);
}

void LooperEngine::stop()  { if (capacity_) req_.store(Req::Stop); }
void LooperEngine::play()  { if (capacity_) req_.store(Req::Play); }
void LooperEngine::clear() { if (capacity_) req_.store(Req::Clear); }

void LooperEngine::undo()
{
    if (!undoReady_.load() || buf_.empty() || undo_.empty()) return;
    /* The two buffers trade places. Which segment the audio thread reads
     * changes under it, so it is told to keep its hands off for the one
     * block the swap takes -- a few milliseconds of silence, rather than a
     * block made of half of each. */
    copying_.store(true, std::memory_order_release);
    const size_t n = std::min(buf_.size(), undo_.size());
    for (size_t i = 0; i < n; ++i) std::swap(buf_[i], undo_[i]);
    recorded_.store(frames_.load());
    const uint8_t l = layers_.load();
    if (l > 1) layers_.store((uint8_t)(l - 1));
    /* One level: what was just put back cannot be undone again. */
    undoReady_.store(false);
    if (state_.load() == LoopState::Overdub) state_.store(LoopState::Play);
    revision_.fetch_add(1);
    copying_.store(false, std::memory_order_release);
}

void LooperEngine::setGrid(LoopGrid grid) { grid_.store(grid); }
void LooperEngine::setLevel(float gain)
{
    gain_.store(gain < 0.0f ? 0.0f : (gain > 4.0f ? 4.0f : gain));
}
float LooperEngine::level() const { return gain_.load(); }

void LooperEngine::envelope(uint8_t* out, uint32_t n) const
{
    if (!out || !n) return;
    std::memset(out, 0, n);
    const uint32_t len = frames_.load(), rec = recorded_.load();
    if (!len || buf_.empty()) return;
    for (uint32_t b = 0; b < n; ++b) {
        const uint32_t a = (uint32_t)((uint64_t)b * len / n);
        const uint32_t z = std::min(len, (uint32_t)((uint64_t)(b + 1) * len / n));
        int32_t pk = 0;
        for (uint32_t f = a; f < z && f < rec; ++f) {
            const int32_t v = *at(buf_, f);
            const int32_t m = v < 0 ? -v : v;
            if (m > pk) pk = m;
        }
        out[b] = (uint8_t)std::min<int32_t>(255, pk >> 7);
    }
}

uint32_t LooperEngine::read(uint32_t from, int16_t* dst, uint32_t n) const
{
    if (!dst || buf_.empty()) return 0;
    const uint32_t len = frames_.load(), rec = recorded_.load();
    if (from >= len) return 0;
    n = std::min(n, len - from);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t f = from + i;
        dst[i] = f < rec ? *at(buf_, f) : 0;
    }
    return n;
}

bool LooperEngine::requestExport(uint8_t track)
{
    if (!frames_.load()) return false;
    return p.startExport(track);
}

LoopExport LooperEngine::exportState() const
{
    return const_cast<ILooperPlatform&>(p).exportState();
}

std::string LooperEngine::exportName() const
{
    return const_cast<ILooperPlatform&>(p).exportName();
}

// ── the audio thread ───────────────────────────────────────────────────────

void LooperEngine::closeLoop()
{
    uint32_t len = pos_.load();
    if (!len) { state_.store(LoopState::Empty); layers_.store(0); return; }
    if (grid_.load() == LoopGrid::Sync) {
        const uint32_t fpb = p.framesPerBar();
        if (fpb) {
            uint32_t bars = (len + fpb / 2) / fpb;
            if (!bars) bars = 1;
            uint32_t want = bars * fpb;
            if (want > capacity_) want = (capacity_ / fpb) * fpb;
            /* Rounded up, the rest of the bar is silence (recorded_ stops
             * where the playing did); rounded down, the overshoot is cut,
             * which is what pressing the switch a little late means. */
            if (want) len = want;
        }
    }
    frames_.store(len);
    pos_.store(0);
    state_.store(LoopState::Play);
    revision_.fetch_add(1);
}

void LooperEngine::apply(Req r)
{
    const LoopState st = state_.load();
    switch (r) {
    case Req::Foot:
        if (st == LoopState::Empty) {
            frames_.store(0);
            recorded_.store(0);
            pos_.store(0);
            layers_.store(1);
            undoReady_.store(false);
            state_.store(LoopState::Rec);
        } else if (st == LoopState::Stopped) {
            state_.store(frames_.load() ? LoopState::Play : LoopState::Empty);
        } else if (st == LoopState::Rec) {
            closeLoop();
        } else if (st == LoopState::Play) {
            const uint8_t l = layers_.load();
            if (l < kMaxLayers) layers_.store((uint8_t)(l + 1));
            state_.store(LoopState::Overdub);
        } else {
            state_.store(LoopState::Play);
            revision_.fetch_add(1);
        }
        break;
    case Req::Stop:
        if (st == LoopState::Rec) closeLoop();
        if (state_.load() != LoopState::Empty) {
            state_.store(LoopState::Stopped);
            pos_.store(0);
        }
        break;
    case Req::Play:
        if (frames_.load() && st != LoopState::Rec) state_.store(LoopState::Play);
        break;
    case Req::Clear:
        state_.store(LoopState::Empty);
        frames_.store(0);
        recorded_.store(0);
        pos_.store(0);
        layers_.store(0);
        undoReady_.store(false);
        revision_.fetch_add(1);
        break;
    case Req::None:
        break;
    }
}

void LooperEngine::rtInput(const int16_t* in, uint32_t frames)
{
    if (!in || !frames || copying_.load(std::memory_order_acquire) || buf_.empty()) return;
    int32_t pk = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t m = mono_of(in + i * 2);
        const int32_t a = m < 0 ? -m : m;
        if (a > pk) pk = a;
    }
    if (pk > pkIn_.load()) pkIn_.store(pk);

    const LoopState st = state_.load();
    if (st == LoopState::Rec) {
        uint32_t w = pos_.load();
        uint32_t i = 0;
        for (; i < frames && w < capacity_; ++i, ++w) *at(buf_, w) = clip16(mono_of(in + i * 2));
        pos_.store(w);
        if (w > recorded_.load()) recorded_.store(w);
        /* Out of memory is the end of the pass: better a loop as long as the
         * board can hold than a take that stops being recorded in silence. */
        if (w >= capacity_) closeLoop();
        return;
    }
    if (st != LoopState::Overdub) return;
    const uint32_t len = frames_.load();
    if (!len) return;
    uint32_t w = pos_.load();
    uint32_t rec = recorded_.load();
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t* d = at(buf_, w);
        const int32_t old = w < rec ? (int32_t)*d : 0;
        *d = clip16(old + mono_of(in + i * 2));
        if (w >= rec) rec = w + 1;
        if (++w >= len) w = 0;
    }
    recorded_.store(rec > len ? len : rec);
}

void LooperEngine::rtRender(float* out, uint32_t frames)
{
    if (!out || !frames || copying_.load(std::memory_order_acquire)) return;
    if (const Req r = req_.exchange(Req::None); r != Req::None) {
        waiting_.store(0);
        armed_.store(Req::None);
        apply(r);
    } else if (const uint32_t w = waiting_.load()) {
        /* The bar line, counted in the blocks the render is made of. */
        if (w <= frames) {
            waiting_.store(0);
            apply(armed_.exchange(Req::None));
        } else {
            waiting_.store(w - frames);
        }
    }
    const LoopState st = state_.load();
    if (st != LoopState::Play && st != LoopState::Overdub) return;
    const uint32_t len = frames_.load(), rec = recorded_.load();
    if (!len || buf_.empty()) return;
    const float g = gain_.load();
    uint32_t pos = pos_.load();
    int32_t pk = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t s = pos < rec ? *at(buf_, pos) : (int16_t)0;
        const float v = (float)s * (1.0f / 32768.0f) * g;
        out[i * 2] += v;
        out[i * 2 + 1] += v;
        const int32_t a = s < 0 ? -s : s;
        if (a > pk) pk = a;
        if (++pos >= len) pos = 0;
    }
    pos_.store(pos);
    if (pk > pkLoop_.load()) pkLoop_.store(pk);
}

} // namespace crosspad
