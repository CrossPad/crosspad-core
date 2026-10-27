// SPDX-License-Identifier: GPL-3.0-or-later
//
// SongEngine, the harvest: a seq track's freeze recorded out of what is heard
// while the song plays, so ARM can leave a track and the track give its kit
// engine back without the song stopping. The pieces are a freeze's own
// (freezePlan(), pieceKey()): recorded whole as the song passes through
// their window -- the run-in bar played, not kept -- and named as a render
// of them would be, so freezeTrack() then finds every one on the card and
// renders nothing. The state is SongEngine::Impl (SongEngineImpl.hpp).

#include "SongEngineImpl.hpp"

#include <cstdio>

namespace crosspad {

namespace {
constexpr uint32_t kHarvestRingFrames = 32768;   // ~0.75 s at 44.1 kHz: the worker drains it every pass
constexpr uint32_t kHarvestChunkFrames = 4096;   // what one write to the card takes
} // namespace

bool SongEngine::Impl::harvestTrack(uint8_t track) {
    if (track >= kTracks || hTrack_.load() >= 0 || busy_.load()) return false;
    if (play_.load() != Play::Running || !syncing_ || liveOn_.load()) return false;
    std::vector<HarvestPiece> plan;
    uint32_t key = 0, pending = 0;
    int route = -1;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SongTrack& t = song_.tracks[track];
        if (!t.inUse || t.kind != TrackKind::Seq || song_.armed == track) return false;
        route = song_.liveRoute(track);
        if (route != 1 && route != 3) return false;   // the pads' kit and Pitch stay loaded: nothing to free
        if (!t.freeze.empty() && t.freezeKey == seqKey(song_, track)) return false;
        const uint32_t spb = song_.samplesPerBar(sampleRate_);
        const std::vector<FreezePiece> pieces = freezePlan(song_, track, spb);
        if (pieces.empty() || pieces.size() > kHarvestMax) return false;
        for (const FreezePiece& pc : pieces) {
            const auto spans = windowSpans(song_, track, spb, pc.w0 * spb, (pc.w0 + pc.pre + pc.cap) * spb);
            char name[40];
            std::snprintf(name, sizeof(name), "%s/FRZ%08X.wav", kFreezeDir,
                          (unsigned)pieceKey(song_, track, spb, pc, spans));
            HarvestPiece h;
            h.w0 = pc.w0 * spb;
            h.skip = pc.pre * spb;
            h.total = pc.cap * spb;
            h.file = name;
            if (!fsx::exists(song_.clipPath(h.file))) pending |= 1u << plan.size();
            plan.push_back(std::move(h));
        }
        key = seqKey(song_, track);
        fsx::makeDir(song_.clipPath(kFreezeDir));
    }
    /* Every piece already on the card: a freeze that renders nothing. */
    if (!pending) return freezeTrack(track);
    if (!hRing_.alloc(kHarvestRingFrames, kHarvestRingFrames)) return false;
    hScratch_.assign(kHarvestChunkFrames * 2, 0);
    for (size_t i = 0; i < plan.size(); ++i) {
        hW0_[i] = plan[i].w0;
        hSkip_[i] = plan[i].skip;
        hTotal_[i] = plan[i].total;
    }
    hPlan_ = std::move(plan);
    hKey_ = key;
    hRoute_ = (uint8_t)route;
    hWritten_ = 0;
    hWriterFor_ = -1;
    hRecording_.store(-1);
    hDone_.store(-1);
    hAbort_.store(false);
    hDoneCount_.store(0);
    hCount_.store((uint32_t)hPlan_.size());
    hPending_.store(pending);
    if (!p.beginHarvest(hRoute_)) {
        hRing_.release();
        hPlan_.clear();
        return false;
    }
    hTrack_.store((int8_t)track, std::memory_order_release);
    logf(false, "harvest track %u: %u of %u pieces to record", (unsigned)track + 1,
         (unsigned)__builtin_popcount(pending), (unsigned)hPlan_.size());
    return true;
}

int SongEngine::Impl::harvestingTrack(uint32_t* done, uint32_t* total) const {
    const int t = hTrack_.load();
    if (done) *done = t >= 0 ? hDoneCount_.load() : 0;
    if (total) *total = t >= 0 ? (uint32_t)__builtin_popcount(hPending_.load()) + hDoneCount_.load() : 0;
    return t;
}

/* RT: the piece being recorded is dropped -- the song jumped, a mute, the
 * ring full -- and the worker throws its file away. */
void SongEngine::Impl::harvestAbortRt() {
    if (hCur_ < 0) return;
    hCur_ = -1;
    hAbort_.store(true, std::memory_order_release);
}

void SongEngine::Impl::rtHarvest(const float* bus, uint32_t frames) {
    RtSection rt(rtGen_);
    if (hTrack_.load(std::memory_order_acquire) < 0) return;
    const uint32_t gen = hBlockGen_.load(std::memory_order_acquire);
    if (gen == hSeenGen_) return;                 // the song did not play this block
    hSeenGen_ = gen;
    if (hAbort_.load(std::memory_order_acquire)) return;   // the worker has not let go of the last one
    const uint32_t n = hBlockN_;
    const bool muted = seq_.songMapMuted() || (seq_.songRoutesMuted() & (1u << hRoute_));
    if (!hBlockOk_ || n != frames || muted) {
        harvestAbortRt();
        return;
    }
    /* Up to the song's wrap, then on from where it wraps to: one stretch of
     * the song after the other, as heard. */
    const uint32_t k = std::min(hBlockWrapAt_, n);
    if (k && !harvestRun(bus, hBlockPos_, k)) return;
    if (k < n) {
        if (hCur_ >= 0) harvestAbortRt();         // no window runs over the wrap
        hNext_ = hBlockWrapTo_;
        harvestRun(bus + k * 2, hBlockWrapTo_, n - k);
    }
}

/* RT: @p n frames of the harvested engine, song samples [at, at + n). False
 * when the piece in hand was dropped. */
bool SongEngine::Impl::harvestRun(const float* bus, uint32_t at, uint32_t n) {
    if (hCur_ >= 0 && at != hNext_) harvestAbortRt();
    hNext_ = at + n;
    if (hCur_ < 0) {
        if (hRecording_.load(std::memory_order_acquire) >= 0) return true;   // the last one still being written
        const uint32_t pending = hPending_.load(std::memory_order_acquire);
        const uint32_t count = hCount_.load(std::memory_order_relaxed);
        /* From where the piece keeps what it records: its run-in is the
         * song's own, which the engine has just played. */
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t keep = hW0_[i] + hSkip_[i];
            if ((pending & (1u << i)) && keep >= at && keep < at + n) {
                hCur_ = (int32_t)i;
                hRecording_.store((int32_t)i, std::memory_order_release);
                break;
            }
        }
        if (hCur_ < 0) return true;
    }
    const uint32_t a = hW0_[hCur_] + hSkip_[hCur_], b = a + hTotal_[hCur_];
    const uint32_t x = std::max(at, a), y = std::min(at + n, b);
    if (y > x) {
        int16_t tmp[256 * 2];
        for (uint32_t done = 0; done < y - x;) {
            const uint32_t m = std::min<uint32_t>(256, y - x - done);
            const float* src = bus + (x - at + done) * 2;
            for (uint32_t i = 0; i < m * 2; ++i) {
                const float v = src[i] * 32767.0f;
                tmp[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
            }
            if (hRing_.push(tmp, m) < m) {
                harvestAbortRt();
                return false;
            }
            done += m;
        }
    }
    if (at + n >= b) {
        hDone_.store(hCur_, std::memory_order_release);
        hCur_ = -1;
    }
    return true;
}

/* Worker: the piece in hand dropped, its file too; the plan stays. */
static void drop_part(WavWriter& w, const std::string& part) {
    if (w.isOpen()) w.close();
    std::remove(part.c_str());
}

void SongEngine::Impl::cancelHarvest(const char* why) {
    const int t = hTrack_.load();
    if (t < 0) return;
    hTrack_.store(-1, std::memory_order_release);
    p.endHarvest();
    /* RT out of rtHarvest(), so the ring is no one's. */
    const uint32_t g = rtGen_.load(std::memory_order_seq_cst);
    if (g & 1u) while (rtGen_.load(std::memory_order_seq_cst) == g) p.sleepMs(1);
    if (hWriterFor_ >= 0) {
        std::string part;
        {
            std::lock_guard<std::recursive_mutex> l(songMutex_);
            part = song_.clipPath(hPlan_[(size_t)hWriterFor_].file) + ".part";
        }
        drop_part(hWriter_, part);
    }
    hWriterFor_ = -1;
    hRing_.release();
    hCur_ = -1;
    hRecording_.store(-1);
    hDone_.store(-1);
    hAbort_.store(false);
    hPlan_.clear();
    logf(false, "harvest track %u called off: %s", (unsigned)t + 1, why);
}

void SongEngine::Impl::serviceHarvest() {
    const int t = hTrack_.load();
    if (t < 0) return;
    std::string dirPath;
    {
        std::lock_guard<std::recursive_mutex> l(songMutex_);
        const SongTrack& tr = song_.tracks[t];
        const char* why = !tr.inUse || tr.kind != TrackKind::Seq ? "not a seq track"
                        : song_.armed == t                        ? "ARM on it"
                        : song_.liveRoute(t) != hRoute_           ? "its engine changed"
                        : seqKey(song_, t) != hKey_               ? "edited"
                        : nullptr;
        if (why) {
            cancelHarvest(why);
            return;
        }
        dirPath = song_.clipPath("");
    }
    if (play_.load() != Play::Running) {
        cancelHarvest("the song stopped");
        return;
    }
    if (hAbort_.load(std::memory_order_acquire)) {
        if (hWriterFor_ >= 0) drop_part(hWriter_, dirPath + hPlan_[(size_t)hWriterFor_].file + ".part");
        hWriterFor_ = -1;
        hWritten_ = 0;
        while (hRing_.pop(hScratch_.data(), kHarvestChunkFrames)) {}
        hRecording_.store(-1, std::memory_order_release);
        hAbort_.store(false, std::memory_order_release);
        return;
    }
    const int32_t rec = hRecording_.load(std::memory_order_acquire);
    if (rec < 0) return;
    const HarvestPiece& pc = hPlan_[(size_t)rec];
    const std::string path = dirPath + pc.file;
    if (hWriterFor_ != rec) {
        if (!hWriter_.open((path + ".part").c_str(), sampleRate_, 2)) {
            cancelHarvest("card write failed");
            return;
        }
        hWriterFor_ = rec;
        hWritten_ = 0;
    }
    const bool done = hDone_.load(std::memory_order_acquire) == rec;
    while (hWritten_ < pc.total) {
        const uint32_t avail = hRing_.used();
        const uint32_t want = std::min(kHarvestChunkFrames, pc.total - hWritten_);
        if (avail < want && !done) break;           // whole chunks while it records
        const uint32_t n = hRing_.pop(hScratch_.data(), want);
        if (!n) break;
        if (!hWriter_.write(hScratch_.data(), n)) {
            cancelHarvest("card write failed");
            return;
        }
        hWritten_ += n;
    }
    if (!done || hWritten_ < pc.total) return;
    if (!hWriter_.close() || std::rename((path + ".part").c_str(), path.c_str()) != 0) {
        std::remove((path + ".part").c_str());
        cancelHarvest("card write failed");
        return;
    }
    hWriterFor_ = -1;
    hWritten_ = 0;
    hDoneCount_.fetch_add(1);
    const uint32_t left = hPending_.fetch_and(~(1u << rec)) & ~(1u << rec);
    hDone_.store(-1, std::memory_order_release);
    hRecording_.store(-1, std::memory_order_release);
    if (left) return;
    /* Every piece on the card: the freeze lays them on the track, rendering
     * none (runFreeze() finds them by name). */
    logf(false, "harvest track %u: every piece recorded", (unsigned)t + 1);
    hTrack_.store(-1, std::memory_order_release);
    p.endHarvest();
    const uint32_t g = rtGen_.load(std::memory_order_seq_cst);
    if (g & 1u) while (rtGen_.load(std::memory_order_seq_cst) == g) p.sleepMs(1);
    hRing_.release();
    hPlan_.clear();
    freezeTrack((uint8_t)t);
}

} // namespace crosspad
