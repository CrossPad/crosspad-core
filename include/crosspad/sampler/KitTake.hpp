// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file KitTake.hpp
 * @brief What the native engine's take needs from a kit, copied out of it.
 *
 * A take reads every header and every resident head off the card -- seconds
 * on its own worker. It does not read the kit model (`KitInfo`) through the
 * kit manager's current-kit pointer meanwhile: the model is not the worker's.
 * A kit load that finishes meanwhile retires it, and the UI thread frees a
 * retired kit's pads (`releaseRetiredKit()`).
 *
 * So the thread that asks -- the one that knows the kit is current right then
 * -- copies what the take reads into a `TakeSpec`, plain data with no pointer
 * into the model, and hands the copy over. The copy is strings and numbers
 * only: nothing here touches the card (`resolveKitSamplePath()` does, so the
 * sample's name is kept as the kit wrote it and resolved on the worker).
 *
 * `WorkerGate` is the other half: start the worker or leave the request for
 * the one running, with no request lost to a worker on its way out -- a take
 * asked for during another is taken after it.
 */

#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/sampler/SampleStore.hpp"
#include "crosspad/sampler/SamplerKit.hpp"
#include "crosspad/sampletools/SampleFx.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace crosspad {

/// One sample of a pad, as the kit names it.
struct TakeLayer {
    char     sample[SampleStore::kMaxPath] = {};   ///< the kit's own name for it, unresolved
    uint32_t start = 0;
    uint32_t end   = 0;                            ///< 0 = to the end
};

/// One pad: what build_pad() reads, and where its layers are.
struct TakePad {
    uint8_t  mode  = 0;        ///< Pad::playbackMode
    uint8_t  choke = 0;        ///< Pad::choke_group
    int16_t  volume = 100;
    int16_t  pan    = 0;       ///< signed offset from centre, as the kit stores it
    int8_t   pitch  = 0;       ///< semitones
    uint16_t hp = 0, lp = 0;
    bool     reverse = false;
    bool     cycle   = false;
    uint8_t  firstLayer = 0;   ///< index into the owner's layer table
    uint8_t  layerCount = 0;

    SampleFx fx() const { return SampleFx{pitch, hp, lp, reverse}; }
    /// Filters and reverse are rendered into a copy on the card; pitch alone
    /// the engine does itself.
    bool wantsCopy() const { return hp || lp || reverse; }
};

/// A whole kit for a take. ~11 kB: the platform keeps it off internal RAM.
struct TakeSpec {
    /// A take holds at most this many samples (one store slot each).
    static constexpr uint16_t kMaxLayers = SampleStore::kMaxSlots;

    int      kitId = -1;                            ///< the kit manager's id, for the verbs
    char     kitPath[SampleStore::kMaxPath] = {};   ///< the kit JSON's logical path
    char     name[40] = {};
    uint16_t layerCount = 0;
    uint16_t layersDropped = 0;   ///< past kMaxLayers, or a name too long to keep
    TakePad  pads[SamplerKit::kPads];
    TakeLayer layers[kMaxLayers];
};

/// One pad for a rebuild from the editor. ~1.5 kB.
struct TakePadSpec {
    int       kitId = -1;
    char      kitPath[SampleStore::kMaxPath] = {};
    uint16_t  layersDropped = 0;
    TakePad   pad;
    TakeLayer layers[KitPad::kMaxLayers];
};

/**
 * @brief Copy @p kit into @p out for a take. No I/O.
 * @return false for a kit no take plays: not parsed, or a pitched kit (it has
 *         an engine of its own). @p out is then left empty.
 */
bool buildTakeSpec(const KitInfo& kit, int kitId, TakeSpec& out);

/// Copy pad @p pad of @p kit for a rebuild. No I/O. False as buildTakeSpec(),
/// or for a pad that does not exist.
bool buildTakePadSpec(const KitInfo& kit, int kitId, uint8_t pad, TakePadSpec& out);

/// The logical path of a layer's file -- resolveKitSamplePath(), which asks
/// the card which layout the kit uses: the worker's to call, not the asker's.
std::string takeLayerLogical(const char* kitPath, const TakeLayer& layer);

/// The rendered copy of a layer the pad's filters or reverse call for
/// (renderedSamplePath()), or "" when the pad wants none. Logical paths.
std::string takeLayerCopy(const TakePad& pad, const std::string& logical);

/**
 * @brief Start a worker, or leave the request to the one that is running.
 *
 * A request is post(): true means no worker is running and the caller must
 * start one (and call failed() if it cannot). The worker loops on next(),
 * which consumes the request it is about to serve and, when there is none,
 * marks the gate idle in the same step -- so a post() that lands while the
 * worker is leaving is either seen by its next() or finds no worker and
 * starts another. Never both, never neither. What the request carries is
 * the caller's (newest wins: a second post() before the worker looks is one
 * pass, which reads the newest payload).
 *
 * Lock-free; any thread may post.
 */
class WorkerGate {
public:
    bool post()
    {
        const uint32_t was = st_.fetch_or(kRunning | kPending, std::memory_order_acq_rel);
        return (was & kRunning) == 0;
    }

    /// The worker, before each pass. False: nothing asked for, and the gate is
    /// idle -- the worker exits without touching it again.
    bool next()
    {
        uint32_t s = st_.load(std::memory_order_acquire);
        for (;;) {
            const bool work = (s & kPending) != 0;
            const uint32_t to = work ? (s & ~kPending) : 0u;
            if (st_.compare_exchange_weak(s, to, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
                return work;
            }
        }
    }

    /// The worker post() asked for could not be started.
    void failed() { st_.store(0, std::memory_order_release); }

    bool running() const { return (st_.load(std::memory_order_acquire) & kRunning) != 0; }
    bool pending() const { return (st_.load(std::memory_order_acquire) & kPending) != 0; }

private:
    static constexpr uint32_t kRunning = 1u;
    static constexpr uint32_t kPending = 2u;
    std::atomic<uint32_t> st_{0};
};

/**
 * @brief The take's hand-over: the newest copy, the gate, and a state.
 *
 * The requester offer()s a copy (the one it replaces comes back to be freed)
 * and starts a worker when told to. The worker loops
 * `while (next()) { T* t = take(); if (!t) continue; ...; done(outcome); }`.
 *
 * state() is **derived**, not stored by both sides: busy for as long as the
 * gate is running -- from the requester's post() to the worker's last next()
 * -- and the last outcome after that. Storing it by hand went wrong twice: a
 * worker that skipped the store because a retake was pending, then found that
 * retake already folded into the pass it had just made, left "taking" for
 * good; and a requester that stored "taking" before its post() could be
 * overwritten by a worker finishing the previous kit.
 */
template <class T>
class LatestWork {
public:
    enum : uint8_t { kIdle = 0, kBusy = 1, kReady = 2, kFailed = 3 };

    /// Requester. @p displaced: an older copy no worker took (the caller's to
    /// free). True: no worker is running, start one.
    bool offer(T* item, T*& displaced)
    {
        displaced = pending_.exchange(item, std::memory_order_acq_rel);
        return gate_.post();
    }
    /// Requester, when the worker offer() asked for could not be started: the
    /// copy comes back and the state says failed.
    T* startFailed()
    {
        outcome_.store(kFailed, std::memory_order_release);
        T* left = pending_.exchange(nullptr, std::memory_order_acq_rel);
        gate_.failed();
        return left;
    }

    /// Worker: one more pass? False: none owed, and the worker leaves.
    bool next() { return gate_.next(); }
    /// Worker: the newest copy, or null when it was taken by the pass before.
    T* take() { return pending_.exchange(nullptr, std::memory_order_acq_rel); }
    /// Worker: how its pass ended, before it asks next().
    void done(uint8_t outcome) { outcome_.store(outcome, std::memory_order_release); }

    /// Anyone else changing what the worker last reported (a release).
    void settle(uint8_t outcome) { outcome_.store(outcome, std::memory_order_release); }

    uint8_t state() const
    {
        return gate_.running() ? uint8_t(kBusy) : outcome_.load(std::memory_order_acquire);
    }
    bool busy() const { return gate_.running(); }

private:
    std::atomic<T*>      pending_{nullptr};
    WorkerGate           gate_;
    std::atomic<uint8_t> outcome_{kIdle};
};

/**
 * @brief How much of the native engine's arena a borrower must leave it.
 *
 * The pool still to grow to its ceiling, page by page (a SMPL_POLY streaming
 * raise), plus @p heads heads of @p headBytes (a pad edit that adds layers).
 */
inline size_t arenaLendKeep(size_t poolCeiling, size_t poolHeld, size_t headBytes, uint32_t heads)
{
    const size_t growth = poolCeiling > poolHeld ? poolCeiling - poolHeld : 0;
    return growth + static_cast<size_t>(heads) * headBytes;
}

/**
 * @brief May @p bytes be lent out of the arena?
 *
 * Judged on the **largest free block**, not the free total. A loan is carved
 * out of one block, and what the owner wants after it is one block too -- a
 * 64 kB pool page, a head of a quarter megabyte. An arena whose free bytes
 * add up but lie in pieces can give neither, and the total said yes anyway.
 * So the loan and everything kept must fit in the largest block together.
 */
inline bool arenaMayLend(size_t bytes, size_t largestFree, size_t keep)
{
    return bytes <= largestFree && largestFree - bytes >= keep;
}

} // namespace crosspad
